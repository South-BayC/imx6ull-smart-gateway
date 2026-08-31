# 云端精判服务（YOLOv8 人员检测 + 人脸白名单比对）

接收网关上传的 RGB565 帧，两级判定：
1. **YOLOv8n** 判定画面中是否有**人员**；
2. 有人时用 **face_recognition**（dlib）做人脸比对 `whitelist/` 白名单。

返回 JSON 结论，设备端按结论告警。
对应设备端：`core/lvgl/src/cloud_detect.c`（识别模式=云端精判 时启用）。

## 安装（VM/主机）

新 Ubuntu（Python 3.12+）pip 受 PEP 668 管控，**必须用虚拟环境**，且 `face_recognition`
依赖的 `pkg_resources` 在 setuptools≥81 已被移除，需先钉住旧版（08-30 板测实证）：

```bash
# ① 虚拟环境（一次性）
python3 -m venv ~/cloud-venv
source ~/cloud-venv/bin/activate          # 每次开新终端跑服务前都要先执行

# ② 先装 setuptools<81（否则 face_recognition 报"Please install face_recognition_models"，
#    实际是 pkg_resources 缺失，报错信息有误导性）
pip install "setuptools<81" -i https://pypi.tuna.tsinghua.edu.cn/simple

# ③ 依赖清单
cd cloud
pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple

# ④ 验证（应打印 FR OK；pkg_resources 弃用警告无害）
python -c "import face_recognition; print('FR OK')"
```

说明：
- `yolov8n.pt`（6.2MB）已预置在本目录并随仓库分发，**运行不依赖外网**；
  若需更换版本，从 https://github.com/ultralytics/assets/releases 下载后覆盖同名文件
- `face_recognition` 依赖 dlib，pip 安装时需本地编译数分钟（需 cmake/g++，
  编译机已具备）；**模型权重随 pip 包内置，安装后运行不依赖外网**
- 若 dlib 编译失败：服务仍可运行（人员检测正常），但所有人按陌生人处理（不漏报）

## 运行

```bash
cd cloud
uvicorn server:app --host 0.0.0.0 --port 8000
```

确认监听：`ss -tlnp | grep 8000`

## 白名单管理

每人一张**露脸正面照**（jpg/png），文件名即姓名：

```
cloud/whitelist/张三.jpg
cloud/whitelist/李四.png
```

- 目录文件有增删/修改，服务**自动重载**（无需重启），控制台打印
  `[cloud] 白名单已加载 N 人: ...`
- 也可用 HTTP 添加（与目录投放等效）：
  `curl -X POST "http://127.0.0.1:8000/whitelist/add?name=张三" --data-binary @张三.jpg`
- 查看已加载：`curl http://127.0.0.1:8000/whitelist`
- 白名单为空 / face_recognition 不可用时，人员一律按**陌生人**处理（fail-safe 不漏报）
- 比对容差 `FACE_TOLERANCE = 0.6`（server.py 顶部可调，越小越严）

## 协议

```
POST /detect?w=<宽>&h=<高>
Content-Type: application/octet-stream
body   = RGB565 原始帧（w*h*2 字节，小端，行连续）

响应 200: {"person": bool, "known": bool, "names": [姓名...], "type": str, "count": N, "conf": F}
```

`type` = 本帧最高置信度检测类别的归一键：`person`（人）/ `animal`（猫狗等动物）/
`object`（其他移动物）。设备端映射为"人员/动物/物体"写入时间轴。

| 云端结论 | 条件 | 设备端行为（统一两级管线，云端=复核定案） |
|---|---|---|
| 非人员 | `person=false`（type=animal/object/空） | 不告警；若本地已告警→**自动消警**（本地误报被压掉）；时间轴记录类型 |
| 已授权 | `person=true, known=true` | 白名单命中；若本地已告警→**自动消警**；时间轴"告警解除" |
| 陌生人 | `person=true, known=false` | STRANGER 告警（本地未告警则升级） |
| 不可达 | 非 200/超时/连接失败 | fail-safe：本地未告警且未检出人脸→INTRUDER 告警；已告警→维持（不漏报） |

设备端为**两级管线**：本地 SCRFD 先初判（检出人脸立即告警/未检出仅轻提醒），
云端复核定案（白名单自动消警/陌生人确认/误报撤销）。`GET /whitelist`（查看白名单）、
`POST /whitelist/add?name=姓名`（添加）。

## 防火墙

VM 内开放 8000 端口（或临时关闭 ufw）：`sudo ufw allow 8000/tcp`

## 板端地址

`core/lvgl/src/cloud_detect.c` 的 `CLOUD_DETECT_SERVER`（默认 `192.168.3.26:8000`，
即 VM 的 IP——与 NFS/TFTP 同机）。修改后需重编应用。

注：设备端 v1 不解析返回的 `names` 中文姓名（字库为业务子集，任意姓名可能缺字），
时间轴统一显示"已授权：人员"；姓名解析留到云端管理页批次。
