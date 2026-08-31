"""
云端精判服务 — 接收网关上传的 RGB565 帧，两级判定：
  1) YOLOv8n 人员检测（COCO person）
  2) 有人时 face_recognition 人脸比对白名单（cloud/whitelist/ 目录）
返回 JSON 结论

协议:
    POST /detect?w=<宽>&h=<高>
    body = RGB565 原始帧数据（w*h*2 字节，小端，行连续）
    响应: {"person": bool, "known": bool, "names": [str], "type": str, "count": int, "conf": float}
          person=false             → 画面无人员（type=animal/object 时说明变动成因）
          person=true, known=true  → 有人员且命中白名单（已授权，设备自动消警）
          person=true, known=false → 有人员但不在白名单（陌生人，设备告警）
          type: "person"/"animal"/"object"（最高置信度检测类别归一）
    GET  /whitelist           → 查看已加载白名单 {"names":[...]}
    POST /whitelist/add?name=姓名   body=一张人脸照片(jpg/png) → 添加白名单

白名单: cloud/whitelist/<姓名>.jpg（每人一张露脸照片，文件名即姓名）。
        目录有变化服务自动重载，无需重启。

运行（VM/主机）:
    pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple
    uvicorn server:app --host 0.0.0.0 --port 8000
    （face_recognition 依赖 dlib，pip 安装时需编译数分钟，需 cmake/g++；
      模型权重随 pip 包内置，安装后运行不依赖外网）

告警语义: 设备端 fail-safe——服务不可达时按"有运动未确认"告警（不漏报）；
          face_recognition 未安装/白名单为空时，人员一律按陌生人处理（不漏报）。
"""
import os
import numpy as np
from fastapi import FastAPI, Request

app = FastAPI(title="imx6ull-gateway 云端精判", version="1.1")

try:
    from ultralytics import YOLO
    _model = YOLO("yolov8n.pt")        # COCO 预训练（首跑自动下载 yolov8n.pt）
    _MODEL_OK = True
except Exception as e:                 # 模型/网络异常时服务不崩，逐请求降级
    print("[cloud] YOLO 模型加载失败:", e)
    _model = None
    _MODEL_OK = False

try:
    import face_recognition            # dlib 人脸检测+128 维特征编码
    _FR_OK = True
except BaseException as e:             # 含 SystemExit：face_recognition 缺模型包时自身 quit()
    print("[cloud] face_recognition 不可用（人员将按陌生人处理）:", e)
    _FR_OK = False

PERSON_CLS = 0                         # COCO 类别 0 = person
ANIMAL_CLS = {14, 15, 16, 17, 18, 19, 20, 21, 22, 23}   # 鸟猫狗马羊牛象熊斑马长颈鹿
FACE_TOLERANCE = 0.6                   # 白名单比对容差（欧氏距离，越小越严；官方默认 0.6）
WHITELIST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "whitelist")


def _norm_type(cls_id: int) -> str:
    """COCO 类别 → 入侵类型键（person/animal/object，设备端映射中文显示）"""
    if cls_id == PERSON_CLS:
        return "person"
    if cls_id in ANIMAL_CLS:
        return "animal"
    return "object"

# 白名单缓存（首次请求/目录变化时自动重载；worker 内单线程调用，无需加锁）
_wl_encs = []
_wl_names = []
_wl_sig = None


def _rgb565_to_bgr(body: bytes, w: int, h: int) -> np.ndarray:
    """RGB565 字节流 → HxWx3 BGR uint8（ultralytics 约定 numpy 输入为 BGR）"""
    px = np.frombuffer(body[: w * h * 2], np.uint16).reshape(h, w)
    r = ((px >> 11) & 0x1F).astype(np.uint8) << 3
    g = ((px >> 5) & 0x3F).astype(np.uint8) << 2
    b = (px & 0x1F).astype(np.uint8) << 3
    return np.dstack([b, g, r])


def _load_whitelist():
    """扫描 whitelist/ 目录（<姓名>.jpg），目录内容/修改时间有变化才重新编码"""
    global _wl_encs, _wl_names, _wl_sig
    if not os.path.isdir(WHITELIST_DIR):
        return
    files = sorted(f for f in os.listdir(WHITELIST_DIR)
                   if f.lower().endswith((".jpg", ".jpeg", ".png")))
    sig = tuple((f, os.path.getmtime(os.path.join(WHITELIST_DIR, f))) for f in files)
    if sig == _wl_sig:
        return
    encs, names = [], []
    for f in files:
        name = os.path.splitext(f)[0]
        try:
            img = face_recognition.load_image_file(os.path.join(WHITELIST_DIR, f))
            e = face_recognition.face_encodings(img)
            if e:
                encs.append(e[0])
                names.append(name)
            else:
                print("[cloud] 白名单照片未检出人脸(忽略):", f)
        except Exception as ex:
            print("[cloud] 白名单照片加载失败(忽略):", f, ex)
    _wl_encs, _wl_names, _wl_sig = encs, names, sig
    print("[cloud] 白名单已加载 %d 人: %s" % (len(names), " ".join(names) if names else "(空)"))


@app.post("/detect")
async def detect(request: Request):
    w = int(request.query_params.get("w", 630))
    h = int(request.query_params.get("h", 340))

    body = await request.body()
    need = w * h * 2
    if len(body) < need:
        return {"person": False, "known": False, "names": [], "type": "",
                "count": 0, "conf": 0.0, "error": "short body"}

    if not _MODEL_OK:
        # 服务降级：无模型时按"未确认"处理（设备端 fail-safe 会告警）
        return {"person": False, "known": False, "names": [], "type": "",
                "count": 0, "conf": 0.0, "error": "model unavailable"}

    img = _rgb565_to_bgr(body, w, h)

    # ---- 1) 目标检测（不过滤类别：人员+动物+其他移动物，解释"画面变动"成因） ----
    person_count, person_conf = 0, 0.0
    top_type, top_conf = "", 0.0
    results = _model.predict(img, conf=0.35, verbose=False)
    for r0 in results:
        if not len(r0.boxes):
            continue
        clss = r0.boxes.cls.cpu().numpy()
        confs = r0.boxes.conf.cpu().numpy()
        person_count = int((clss == PERSON_CLS).sum())
        if person_count:
            person_conf = float(confs[clss == PERSON_CLS].max())
        i = int(np.argmax(confs))
        top_type = _norm_type(int(clss[i]))
        top_conf = float(confs[i])

    if not top_type:
        return {"person": False, "known": False, "names": [], "type": "",
                "count": 0, "conf": 0.0}

    # 动物/其他移动物：不算人员入侵，返回类型说明（设备端仅时间轴，不告警）
    if top_type != "person":
        return {"person": False, "known": False, "names": [], "type": top_type,
                "count": 0, "conf": top_conf}

    # ---- 2) 白名单比对（检出人员才做人脸识别，省算力） ----
    known, names = False, []
    if _FR_OK:
        _load_whitelist()
        if _wl_encs:
            rgb = img[:, :, ::-1].copy()          # face_recognition 按 RGB 通道序
            locs = face_recognition.face_locations(rgb)
            if locs:
                encs = face_recognition.face_encodings(rgb, locs)
                for e in encs:
                    d = face_recognition.face_distance(_wl_encs, e)
                    i = int(np.argmin(d))
                    if d[i] <= FACE_TOLERANCE:
                        known = True
                        if _wl_names[i] not in names:
                            names.append(_wl_names[i])

    return {"person": True, "known": known, "names": names,
            "type": "person", "count": person_count, "conf": person_conf}


@app.get("/whitelist")
async def whitelist_list():
    """查看当前已加载的白名单"""
    if not _FR_OK:
        return {"names": [], "error": "face_recognition unavailable"}
    _load_whitelist()
    return {"names": _wl_names}


@app.post("/whitelist/add")
async def whitelist_add(request: Request, name: str = ""):
    """添加白名单：body=一张露脸照片(jpg/png)，name=姓名（与目录投放等效）"""
    if not name:
        return {"ok": False, "error": "name required"}
    if not _FR_OK:
        return {"ok": False, "error": "face_recognition unavailable"}
    os.makedirs(WHITELIST_DIR, exist_ok=True)
    with open(os.path.join(WHITELIST_DIR, name + ".jpg"), "wb") as f:
        f.write(await request.body())
    return {"ok": True, "name": name}
