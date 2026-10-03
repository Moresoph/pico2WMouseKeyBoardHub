# ESP32 + Pico 2 W 热点网页交接

更新日期：2026-10-01。最后一次用户确认：11:42，键盘已经能连接。

**Git 更新（2026-10-03）：** `v0.6.3` 对应 `bd112dc`；
0.6.4 删除修复作为后续独立提交保存在 `classic-hid-output`。
用户已要求提交并推送；继续工作请使用该分支的最新提交。
下方“尚未提交/推送”和早期 HEAD 信息均为当时的交接记录，
不能当成当前 Git 状态。没有改写 `v0.6.3` 标签。

## 19:33 后删除操作冲突修复（19:46 已部署）

19:45 用户换到另一个 USB 口后 CH340 变成 **COM16**，连续读取成功；
实际观测到 M585/M590 在 saved input 3、active、HID=1 encrypted=1、
441 个鼠标报告；M720 record 2仍保留/inactive，键盘也已加密就绪。
因此“新鼠标始终未入录”已被新实机证据更新；触发自动按键还是网页操作
没有抓到，不要替用户虚构触发路径。

随后已在 COM16 核MAC、NVS备份、刷verify **0.6.4-delete-controls**，
NVS启动前字节保持不变：
`backups\esp32-nvs-before-delete-controls-20261003-194611.bin`、
`backups\esp32-nvs-after-delete-controls-before-boot-20261003-194611.bin`。
最初USB重枚举短暂失败，之后重读成功确认运行0.6.4，三条旧/新输入
记录都在，M585名称已持久，键盘112报告、M585401报告均HID/加密就绪，
鼠标实际7.5ms、storage-fault=0、UART/PC telemetry fresh。
Pico未刷，仍0.6.0。没有真实删除用户记录做测试。

**以下“尚未部署/USB缺失”的叙述是19:33时的历史阻塞，不是最新状态。**

用户继续反馈网页不能删设备。已查明代码层面：长按立即开启自动扫描，
而旧 UI 把自动扫描/扫描中的删除按钮锁住，授权后仍需手动停止扫描。
这使删除操作不直观；实时ESP32串口不可用，具体用户按钮状态未取得截图。

修复版 `0.6.4-delete-controls`：有物理授权且没有正在试配时，
自动扫描不再禁用删除按钮。确认输入删除后先停扫描，最多等6秒，
再重新核token与准确地址执行原删除路径。扫描停不下来、授权到期、
取消或记录identity变更均保留记录并报未执行；PC删除也停止自动扫描。
新增`deletion_pending`状态锁住其他扫描/录入，取消仍可用。
取消未开始的BLE扫描参数回调不会再次启动扫描。

生产函数原生回归覆盖扫描中删除、旧鼠标空闲/活动删除路径、超时、
取消、identity变更保护；12浏览器场景和ESP32构建通过。
待刷镜像 `build\esp32-slots-buttons\pico_dual_bt_input.bin`，
SHA256 `07EEB8104E9A0A50D95BF36EED9395770CF6C0775686B287D2A627CFAA908312`。
**尚未刷写**：两次检测只见Pico COM5，无CH340；
ESP32板上仍0.6.3，Pico仍0.6.0。现有板上操作可通过
长按授权→“停止自动录入／手动管理”→扫描停止→确认删除。

之前当前代码已在commit `bd112dc`、本地annotated tag `v0.6.3`，
0.6.4删除修复作为之后的独立提交，不修改该release标签。
0.6.3实际部署应用保存于ignored
`backups\esp32-v0.6.3-deployed-before-delete-controls.bin`；
当前build与ESP32板上程序均为0.6.4，不要与此前0.6.3 hash混淆。
未删除用户真实设备作验证，未刷Pico，未动任何keys。

## 2026-10-03 19:22 自动录入、名称、删除按钮（最新）

用户明确要求“按键打开录入后，发现新鼠标就自动配对”，同时反馈删除
按钮不清楚、输入只显示 MAC。已实现并刷入 ESP32 `0.6.3-auto-input`，
Pico 没有刷，仍 `0.6.0-slots-buttons`。

长按 BOOT 三秒松开自动排队 BLE 扫描，再扫描 Classic，结束后对唯一、
明确键盘/鼠标类型、未保存的候选自动试配。排除 Pico/hub 名称与旧地址；
多个新同类候选或未广播明确角色时必须网页手选，不凭 RSSI 或列表顺序。
一把新鼠标/新键盘可顺序录入；未知连接没有窗口仍拒绝。旧 trial
加密/实际 HID 角色验证、失败回退/FIFO 机制复用。

网页增加“停止自动录入／手动管理”；删除/重命名前先使用这个按钮。
删除文字保留高对比度，禁用原因显示在旁边，活动输入也有删除按钮。
保存记录升级 v2：388 字节 CRC registry，含每个输入 64 字节名称；
兼容迁移 v1 68 字节和原两输入记录，保持地址、active、order、bond。
广播/HID 名称会更新并保存，断线重启仍有名称。未知名称显示“名称尚未
获取”，也可授权设置名称。旧 v1-only固件不能直接读取v2，不要盲降级/擦 NVS。

最终镜像：

```text
build\esp32-slots-buttons\pico_dual_bt_input.bin
SHA256 A275AE4557A7CC464A4BC548C7DF20F700F4F9251783A1ACD2D873D6A8BD371C
```

已核 MAC、write/verify，NVS 在写前/写后/启动前 byte-identical。
备份：`backups\esp32-nvs-before-auto-input-final-20261003-192127.bin`、
`backups\esp32-nvs-after-auto-input-final-before-boot-20261003-192127.bin`。
不是修改/擦除密码或配对存储。新增 USB `enroll [auto|manual|cancel]`
复用相同入口，供用户明确授权的诊断；HTTP不能自行开授权。

原生测试覆盖实际生产自动扫描/唯一候选入录、多键鼠顺序、歧义、已知/hub
排除、namepersist/rename、CRC/v1迁移、旧记录失败回退；11浏览器场景覆盖
删除按钮非空、名称/fallback、auto/manualdisabled提示。测试通过。

实机 `enroll auto` 跑过有界自动循环，确认 BLE→Classic、等待旧worker、
排除 Pico、本机registry无丢失；该轮 M585/M590 已无配对广播，没有真
新鼠标入录成功证据。测试结束用 `enroll cancel` 停窗口，保留旧记录。
最终二次刷写启动已确认 AP/HTTP，读取再次遇 CH340 access-denied。
下一步让鼠标重新进入配对再长按，无需网页点“作为鼠标”，除非候选歧义。
用户未响应物理操作请求、没有切电脑Wi-Fi授权；不重复刷写/长时间轮询。
源码仍未 commit/push。运行时灯 ESP32 包含新故障灯，Pico仍旧。

## 18:35 后热点密码请求（最新）

**18:56 更新：用户重新插拔后 COM15 已恢复，设置命令和随后的 `web`
读取均确认用户请求的本机密码、SSID PicoHub-9235、热点 ON。此项已完成，
不需要再刷写或修改密码。** 下文串口阻塞是此前记录，不是当前结果。

用户要求将热点密码简化。代码新增 USB-only
`web password <8..63 printable ASCII>`，保存至 `hub_web/ap_password`、
commit/readback 验证，再重启 AP 应用，不改变蓝牙配置；没有把实际密码硬编码
在源码中。输入长度/ASCII 边界和原回归通过。

带该命令的 ESP32 镜像已核身份、应用区刷入/verify，通过写入前后 NVS 比对：
SHA256 `54735CFEBC3A1E23D0D7FE47E52A4E6F76B8E8674E75BD3612341A759EDC790D`。
**密码变更尚未执行成功**：重启后 COM15 消失，初次命令及三次有界重试均
无法打开端口，未发送设置命令。原已保存热点密码仍应保留；不能因为固件刷入
就说密码已变。等 USB 恢复，执行用户在对话里指定的密码，然后 `web` 核实
ON 和实际密码。不要重复刷程序或清 NVS，Pico 未动。

## 2026-10-03 16:58 新鼠标录入修复（优先于以下旧记录）

用户强调必须支持新鼠标，不能固定使用 M720。已修复阻碍录入的路径：
旧设备重连中的扫描请求不再直接拒绝，改为排队并在 SDK worker 结束后
自动扫描；管理期间停止新的离线后台重连（试配所选角色除外）。
候选列表保留名称/HID 并在容量满时优先保留 HID 候选；不按 RSSI 猜选。
选中目标、等待旧连接结束、验证和恢复阶段有中文状态与真实地址；
新目标实际开始连接时取得新的 120 秒配对预算。
试配失败恢复旧记录时同时关闭管理，保证旧设备重连不继续被窗口阻止。
网页配对码输入不再被每两秒刷新清空。

新测试 `tests\input_enrollment_test.c` 通过
`tests\generate_enrollment_test.cmake` 抽取**实际生产函数**，
SDK/NVS/队列边界 mocked；验证不是 M720 的鼠标录入、加密与角色门禁、
旧记录/键盘保留、切回旧鼠标、取消/到期/连接失败/错角色/写失败回退、
scan 排队、失效 token 和 candidate 地址拒绝。9 个浏览器场景通过，
包括试配地址、扫描排队和验证码跨刷新保留。

ESP32 已刷入最终 `0.6.2-input-enrollment`：

```text
build\esp32-slots-buttons\pico_dual_bt_input.bin
SHA256 E9A7DBD37C3863A0B807147BCC0EA9DD627DCD54B3B113227D05F5B1C3596AF5
```

MAC 已核为 `10:52:1C:1D:92:34`，verify 成功，
`backups\esp32-nvs-before-enrollment-final-20261003-165724.bin` 与
`backups\esp32-nvs-after-enrollment-final-before-boot-20261003-165724.bin`
在启动前 byte-identical。没有清旧 M720、键盘或 PC 配对。
首次 0.6.2 runtime 状态确认两个旧记录 active、storage-fault=0、
UART/PC telemetry 在线；最后一次回退修正在刷后读串口时又遭遇 CH340
access-denied。这是控制台验证阻塞，不等于 verify 没成功。
随后最后一次 COM15 只读重试成功，最终 0.6.2 runtime 已确认：
两个旧记录 active、storage-fault=0、UART/PC telemetry fresh；两输入当时
仍在旧目标重连，并非已录入新鼠标。不要把中间串口失败当作最后状态。

Pico 本轮**未刷**，仍是 `0.6.0-slots-buttons`、7 PC slots；
PC1 encrypted/HID-ready、7.5ms、buffer OFF、UART framing-errors=0。
ESP32 的新故障灯随 0.6.2 部署；Pico 新故障灯0.6.1仍仅构建、未部署。

**未获得新鼠标的实机配对证据**，用户未提供具体新鼠标/候选选择状态；
管理电脑仍连原 Internet Wi-Fi，没有切网授权。不要声称“这只新鼠标已配好”。
下一步用手机进入热点，长按 BOOT 松开授权→扫描 BLE/Classic→选择新鼠标；
看排队/目标/操作结果，不要先删除 M720。只需读必要状态，避免又扩大任务
或反复刷写。源码仍未提交/推送。

## 2026-10-03 槽位与按键更新（最新，优先于以下旧记录）

用户已授权刷入新操作逻辑，并要求保存 5 个输入记录、7 台电脑；
支持网页删除，满槽淘汰最老记录。按“扩大保存数量”实现：ESP32 仍同时
启用一把键盘和一只鼠标；Pico 保存 7 台，保留 4 个并发 peripheral 限制，
不声称全部 12 个设备同时连接。

两块板已刷入 `0.6.0-slots-buttons`，源码未 commit/push。
ESP32 目前通常 COM15，Pico COM5。运行时按键：
ESP32 BOOT 短按提示/取消，双击开关热点，长按 3 秒**松开后**
打开 120 秒管理窗口；Pico BOOTSEL 短按切下一台 ready PC/取消配对，
长按 3 秒松开开电脑验证码配对。没有长按清空所有记录。
正常灯熄灭，其余保持重连单闪、配对慢闪、输入码快闪。
用户在 16:20 要求故障改为常亮 2 秒、快闪 2 秒循环：源代码已统一修改，
快闪亮灭各 100 ms，并从故障提示开始时计时；是否刷入见后续验收记录，
不要把新源码灯态直接当成此前 `0.6.0` 已部署镜像的灯态。

故障灯代码已编译为 `0.6.1-fault-lamp`，边界/进入退出/计时回绕和相关
回归通过，**未刷入任何一块板**：当前只检测到 Pico COM5，ESP32 USB
未出现。两块板最后实际部署仍是 `0.6.0`。
同一 build 路径现在是待刷的 0.6.1：
ESP32 SHA256 `880386C298911D1BCD73EC09C5CD15E57BCA186F81E84639BA69DCBE4B1C5AF0`，
Pico UF2 SHA256 `5855ED27B13BCA68A568D0567DD7EB8C61E0EE78E56030DA35222F891A208755`。
实际部署的旧 0.6.0 镜像已另外保存在 ignored backups：
`esp32-0.6.0-slots-buttons-deployed-20261003.bin`、
`pico-0.6.0-slots-buttons-deployed-20261003.uf2`。下文 0.6.0 hash 是历史部署
身份，不能拿当前 build 文件直接匹配。

网页新增 5 个已保存输入、7 个电脑槽、扫描候选及激活/删除，
必须实体授权＋窗口 token＋同源 JSON 请求。输入试配加密/HID 角色
成功后才保存/淘汰；取消失败恢复旧记录。FIFO 是首次录入顺序，不是
最近使用时间。PC 删除跨 UART 确认，未收到应答不能报告成功。
旧输入 NVS 与旧 PHO1 四槽记录有迁移；初始旧顺序按原槽号。

新入口：`include\hub_input_records.h`、`include\hub_ui.h`、
`tests\ui_records_test.c`、`tests\pc_slots_test.c`；
其余入口延续下文。当前 IDF transport 枚举 **Classic=0、BLE=1**，
新 registry 对它做 compile-time guard。第一轮迁移发现错误枚举，
已经修正并加回归测试、重新刷入，旧 NVS 字节不变，不要再换为 1/2。

最终镜像：

```text
build\esp32-slots-buttons\pico_dual_bt_input.bin
6A4E4D8D64C1E20A1EF9AF1ECC2C9A480E3F68A6AE16C99D897268668201E7B9

build\dual-slots-buttons\pico_bt_hub.uf2
D0259472D9C19493E7ED9C59127166D13AC281DE0860A81F9BD55CB8DE0F100F
```

ESP32 只刷应用，verify 成功，NVS 24,576 字节在写入前后、启动前完全
一致。Pico 校验 2,088 程序块、893 个非程序扇区未变。
Pico runtime 已确认 7 槽、原 PC1/PC2 身份保留、PC1 encrypted/HID-ready、
7.5 ms、buffer OFF、UART framing-errors=0。
相关原生和浏览器回归通过；生产 PC 删除的身份守卫也有测试。

**仍未全部实机验收：** CH340 在启动/读取时再次发生端口消失或访问拒绝，
最终 ESP32 状态不能稳定读出。电脑现在只有正在使用的
`ChinaUnicom-7FA8-5G` Wi-Fi 上网，无有线 Internet；用户未明确允许临时
切网络，因此没有擅自切到热点。需要手机连接 PicoHub，按键授权后验证
网页、新配对、取消回退、删除与灯态。没有删除真实已配对记录做测试。
不要声称最终键盘回连或真实满槽淘汰已经过硬件验收。

关键备份：

```text
backups\esp32-nvs-before-slots-migration-fix-20261003-155527.bin
backups\esp32-nvs-after-slots-migration-fix-before-boot-20261003-155527.bin
backups\pico-f1812912a883cce2-before-slots-20261003-155209.bin
backups\pico-f1812912a883cce2-after-slots-before-boot-20261003-155209.bin
```

这些可能含密钥，不提交、不上传。先解决连接/完成只读查看，不因 USB
不稳再次随意刷写。最新构建命令见 README 顶部 0.6.0 章节。

## 20:50 重启重连修复更新（优先于下面的上午记录）

用户晚上要求修复“ESP32 重启后键盘不能自动连接”。当前 ESP32 已改为
`0.5.3-reconnect-recovery`，并刷入核验过身份的板子；Pico 没有再刷写。
本轮源码仍未 commit/push，必须保留当前 dirty checkout。

当前 ESP32 镜像：

```text
build\esp32-reconnect-recovery\pico_dual_bt_input.bin
SHA256 34D305F6C0909AE1E7CFECE2D162AFE20359A21BCA3131CCCA2B62CF12BDFA43
```

与上午镜像相比，已修复的确定代码缺陷：

- 阻塞约 30 秒的 BLE HID open 改到独立 worker；Classic 调度、100 ms
  超时/灯态监视不再被鼠标阻塞。只在同 transport 内串行。
- 普通传输/安全建立超时不再永久 paused：先释放输入、清理旧连接，
  然后按 1/2/4/8/15 秒上限退避重试。真正的密钥/认证错误仍停在安全门禁。
- 只在 OPEN/CLOSE callback 内保存/移除 SDK 所有的 HID 指针；
  不保存可能已被 callback 释放的 open 返回值，未收到 CLOSE 不重复打开。
- 入站 ACL 与出站 OPEN failure 相撞时给 HID 重建机会，三次失败后清理；
  不直接丢掉已建立的入站链路。
- Classic 清理桥在 BTU 线程检查 handle + ACL generation，
  用固定版本 `btm_remove_acl` 断线但不删 bond；
  加密回调用独立 request token 拒绝过期结果。

新代码入口 `esp32\main\reconnect_policy.h` 和
`tests\esp_reconnect_test.c`；已有 `classic_security_bridge.inc` 及其测试
扩展了清理、handle 重用、generation、瞬态/安全错误区分。
`esp32\main\CMakeLists.txt` 对应用 component 增加 `-Os`；
最终大小 `0x175950`，分区保持原样，余量 5,808 字节。

构建、相关回归、Flash verify 都已通过。当前备份：
`backups\esp32-nvs-before-reboot-recovery-final-20261001-204939.bin`，
24,576 字节，ignored，可能包含密钥，不上传。

**实机验收仍受阻，不能宣称已彻底解决。** 最后的两次有界观察中，
键盘仍是寻呼超时 `0x04`，鼠标 `0x85`，两输入均未响应；未进入键盘认证。
实测确认：BLE 阻塞时 Classic 仍持续重试、bonded=1、没有自动暂停、
没有 PIN/新配对窗口、UART/遥测在线。用户不可响应物理唤醒请求。
下一步需要让设备在原配对通道正常唤醒，再验证连续重启后的加密 HID
和真实报告，不要把无回应直接等同于丢密钥或重新配对。

Pico 最后读数仍为 `0.5.2-dual-web-telemetry`，PC1 encrypted/HID-ready，
7.5 ms、buffer OFF、UART online、framing-errors=0。
本次未动热点配置、Pico 固件或鼠标/电脑时序参数。

## 先看这几条

1. **当前板上的程序已更新，但网页相关代码尚未 commit/push。** 新 session 必须复用下面这个本地目录，或先经过用户同意迁移未提交改动；直接从 GitHub 新建 worktree 会缺少本轮代码。
2. **保留当前已经恢复连接的版本。** 不要一开始就重新刷写、重配键盘、清空 NVS 或更改蓝牙参数。
3. 网页、热点、电脑状态回传已经实现，也曾实机访问成功；**最终版本“有热点客户端访问网页时，键鼠是否仍然流畅”尚未验收**。
4. 用户对本轮耗时、反复刷写非常不满。下一步只做有界、必要的验证，不增加功能，不反复轮询或扩大排障范围。

## 1. 工作区与 Git

| 项目 | 当前值 |
| --- | --- |
| 本地仓库 | `F:\copilotSession\pico2WMouseKeyBoardHub` |
| 当前分支 | `classic-hid-output` |
| 工作区类型 | 用户本地 checkout，原地工作，非隔离 worktree |
| HEAD | `9503142` — 公开仓库 README 说明 |
| 已提交的可用双板基线 | `45bc712` |
| 公开远端 | <https://github.com/Moresoph/pico2WMouseKeyBoardHub> |
| 远端分支 | `main`、`classic-hid-output`，之前均推送到 `9503142` |
| 本轮网页改动 | 已在本地文件中，未提交、未推送 |

新 session 优先选择“复用现有目录/分支工作区”，继续使用上述路径。
不要直接切换到 `main`、重置工作区、丢弃未提交文件，或把远端代码误认为板上的最终源码。
用户这次只要求交接文档，没有授权提交本轮网页代码。

## 2. 当前架构、接线与硬件身份

```text
FILCO Classic 键盘 ──蓝牙──┐
                          ├── ESP32 ──UART── Pico 2 W ──BLE HID── PC1…PC4
M720 BLE 鼠标 ─────蓝牙────┘      │
                                └── 自建 Wi-Fi 热点 + 本地只读 HTTP 网页
```

| 连接 | 接线 |
| --- | --- |
| Pico UART TX | GP4 → ESP32 D14/GPIO14 RX |
| Pico UART RX | GP5 ← ESP32 D13/GPIO13 TX |
| 地 | 共地 |
| 串口 | 460800 baud，8N1，3.3 V |
| ESP32 状态灯 | GPIO2，板载蓝灯 |

两板各自 USB 供电，不合接 5 V/3.3 V。GPIO13/14 接线已经可用，不要改回旧的 GPIO2/GPIO15 串口方案。

| 设备 | 身份 |
| --- | --- |
| ESP32 | 原版 ESP32-D0WDQ6，rev 1.1，4 MB |
| ESP32 烧录 MAC | `10:52:1C:1D:92:34` |
| ESP32 蓝牙 MAC | `10:52:1C:1D:92:36` |
| ESP32 AP MAC | `10:52:1C:1D:92:35` |
| ESP32 控制台 | 通常 COM12，CH340，VID:PID `1A86:7523` |
| Pico | Pico 2 W，4 MB，序列号 `F1812912A883CCE2` |
| Pico 控制台 | 通常 COM5，USB CDC，VID:PID `2E8A:0009` |
| 不应刷写的其他串口 | COM10/COM11，CH342，与本任务目标不同 |

输入槽 1：FILCO `00:18:00:3D:38:89`，Classic。
输入槽 2：M720 Triathlon `F9:37:F0:2A:90:27`，BLE random address。
旧单板方案的 M720 地址末尾是 `26`；不要把它当成本轮鼠标通道。

## 3. 板上最终固件

| 板 | 版本 / 文件 |
| --- | --- |
| ESP32 | `0.5.2-softap-status`；`build\esp32-dual-web\pico_dual_bt_input.bin` |
| Pico | `0.5.2-dual-web-telemetry`；`build\dual-web\pico_bt_hub.uf2` |

ESP32 最终应用 SHA256：

```text
B43FB16E5B6F2A32E00EDADF5233852150E1DCA8A467363AF266919690662B3F
```

Pico 最终 UF2 SHA256：

```text
5F75CEBD3477C41C11AE763673732661F6B8061468B08A5825F266F86FC7680D
```

本轮多次修复都沿用 `0.5.2` 标识，**只看版本号不能判断是否为最终镜像**。

ESP32 最终应用大小 `0x175DE0`，现有 app 分区大小 `0x177000`，
只剩 **4,640 字节**。目前可以构建并刷入；后续增加代码必须检查尺寸，
不能擅自调整分区、擦除存储来掩盖溢出。

## 4. 网页与热点怎么使用

| 项目 | 行为 |
| --- | --- |
| 本机实际热点 | `PicoHub-9235`，一般规则是 `PicoHub-XXXX` |
| 网页地址 | `http://192.168.4.1/` |
| 网络模式 | AP-only，不连接路由器，不做 NAT，不提供互联网 |
| 安全 | WPA2-PSK；首次生成独立的 16 字符密码，存于本机 NVS |
| 密码获取 | ESP32 USB 控制台执行 `web` |
| 密码存储 | NVS namespace `hub_web`，key `ap_password` |
| 管理客户端上限 | 两台；不影响四个蓝牙电脑槽的定义 |
| Wi-Fi | channel 6，HT20，100 ms beacon |
| 网页刷新 | 每 2 秒 GET `/api/status`，不重叠请求 |

**实际热点密码不写在交接文档或公开仓库中。** 不要把 `web` 输出中的密码、
Flash/NVS 备份或配对密钥提交、上传。键盘固定配对 PIN `123456`
不是热点密码，仍只用于明确打开的键盘配对窗口。

手机需要连接这个热点；提示“没有互联网”时选择保持连接，然后手动输入
完整 HTTP 地址。没有实现 DNS/captive portal 自动弹网页。
被控制的电脑继续走 Pico 蓝牙，不需要加入管理热点。

ESP32 USB 控制台：

```text
status
web
web off
web on
```

`web off` 关闭 HTTP 和 Wi-Fi，不关闭蓝牙；
`web on` 恢复热点/HTTP。开关为 RAM-only，重启默认开启。
没有任何 HTTP 配对、忘记设备、电脑切换或其他写操作端点。

页面显示输入目标、配对记录、加密/HID/可转发状态、报告数量、
四个电脑槽、当前选中电脑、鼠标缓冲与通信新鲜度。
不显示实际按键、鼠标轨迹或密钥。PC 名称目前是 PC1～PC4。

**最后一次只读检查时，管理电脑的 WLAN 是 disconnected，HTTP 请求超时。**
这不等于 ESP32 服务没有启动；先检查客户端是否连接热点，别立即改固件。
此前给 Windows 当前用户添加过手动连接的 `PicoHub-9235` Wi-Fi profile；
临时含密码 XML 已删除。网站预览必须用系统默认外部浏览器，不用嵌入面板。

## 5. 已确认与未确认

### 已确认

- 两板构建通过；双板协议、路由、HID、电脑状态和加密桥原生回归通过。
- 浏览器测试覆盖正常状态、Pico 状态过期、HTTP 错误、不兼容数据、
  配对存储故障以及设备名称的安全文本显示。
- 实际连接热点后，HTTP/浏览器曾正确显示：
  PC1 已加密且 HID-ready、7.5 ms、当前选中；PC2 已保存但离线；PC3/4 未录入。
- 实际 HTTP 连续读取、静态资源类型、无敏感字段和 POST 拒绝检查通过。
  `web off/on` 曾验证不重启即可恢复，并保持 UART 与 PC1。
- ESP32 仅刷应用区，逐次 verify；未刷 bootloader/分区表，未擦 NVS。
- Pico 全 Flash 备份/读回：2,060 个程序块匹配，895 个非程序扇区未变，
  包括电脑配对存储。
- 最终刷写后的日志：FILCO `HID=1 encrypted=1 bonded=1`，72 个真实报告；
  M720 `HID=1 encrypted=1`，720 个真实报告，实际
  `interval=7.500ms latency=44 timeout=2160ms`。
- 上述检查中两输入均未 paused、未 pending，`allow-unencrypted=0`；
  11:42 用户明确表示“现在键盘能接上了”。

### 不能声称已经确认

- 最终版本在**管理客户端连接并持续访问网页时**的键鼠手感、
  长时间不掉线，以及 SoftAP/Bluetooth 共存稳定性。
- 绝对物理输入到屏幕的延迟。两个 7.5 ms 间隔不代表固定 15 ms 延迟；
  HTTP 响应耗时也不是键鼠延迟。
- 四台电脑同时在线与真实快捷键切换的完整验收。
- 最终成功键盘会话中，GAP 加密事件正常到达；新增的“缺少确认时主动请求加密”
  分支有原生测试，但该分支没有在这次成功会话中被独立强制触发。

## 6. 本轮问题与修复

不要为了网页把加密要求降低或把“有配对记录”当作“本次连接已加密”。

1. Wi-Fi + Classic/BLE 初次链接导致 IRAM 溢出。关闭 Wi-Fi
   IRAM/RX-IRAM 吞吐优化后可构建，保留蓝牙控制器配置。
2. 固定 AP 地址初次设置失败：`ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED`。
   已改为停止 DHCP → 设置 IP → 启动 DHCP。
3. 已保存键盘可能只有入站 ACL，尚未完成 HID。
   旧逻辑把等待其他输入连接的时间算成安全超时并暂停槽。
   现在有五秒 grace；串行连接任务可用时在现有 ACL 上补 HID setup，
   不主动制造第二条 ACL、不开放新的配对窗口。
4. 用户看到过“已连接，未就绪 / HID 就绪 / 未加密 / 报告 0”。
   `HID 就绪` 在当前页面实际表示描述符已映射，不代表可以转发。
   已加密与 mapped 才满足正常转发条件；未确认加密时拒绝转发。
5. 新增固定版本 IDF 加密桥：在 BTU 线程请求 `BTM_SetEncryption`，
   完成时查询 live ACL 和真实 `BTM_SEC_FLAG_ENCRYPTED`。
   成功返回码或保存的 bond 都不能单独证明加密。
   回调通过队列交给应用，匹配 ACL generation，拒绝过期结果。
   普通链路在请求期间消失不应当作认证失败永久暂停。

**特别注意加密桥所有权：** 当前固定 IDF 的 `BTM_SetEncryption`
同步结果也会调用 callback。不能在返回非 `BTM_CMD_STARTED` 时再手动调用一次
完成函数或释放同一个 context。测试覆盖同步、异步、资源失败与过期 generation。
禁止直接把 `p->secure` 设为 true，或自动启用 `open-connect` 绕过问题。

ESP-IDF 共存文档标记：SoftAP 仅发 beacon 与蓝牙是 Y；
SoftAP 正在接客户端或已有客户端，与 Classic/BLE 是 **C1：支持但性能不稳定**。
参考 `.deps\esp-idf\docs\en\api-guides\coexist.rst`。这也是剩余人工验收的重点。

## 7. 代码入口

| 文件 | 作用 |
| --- | --- |
| `esp32\main\main.c` | 输入生命周期、安全门禁、状态快照、UART、加密结果消费 |
| `esp32\main\hub_web.c` / `.h` | AP、密码 NVS、HTTP、只读 JSON、USB `web` 命令 |
| `esp32\main\web\index.html` / `style.css` / `app.js` | 嵌入式手机友好页面、错误与过期显示 |
| `esp32\sdkconfig.defaults` | 共存、Wi-Fi core 1、缓冲、IRAM 与 socket 限制 |
| `esp32\classic_input_policy.cmake` | 生成 SDK overlay；加密要求与私有 API 的版本校验 |
| `esp32\classic_security_bridge.inc` | BTU 线程中的实际 Classic 加密请求/确认 |
| `include\hub_classic_security.h` | 应用与上述桥的接口 |
| `esp32\main\input_policy.h` | 可原生测试的配对、ACL、加密回复门禁 |
| `include\hub_link.h` / `src\hub_link.c` | UART 帧、电脑状态编码与 freshness/replay 检查 |
| `src\hub_uart_input.c` | Pico 输入接收、优先心跳、非阻塞状态发送 |
| `src\hub_output.c` / `include\hub_output.h` | Pico 真实电脑槽快照、选中与 encrypted/HID-ready |
| `tests\link_output_test.c` | 电脑状态协议、过期、重放、跨 session、计时回绕 |
| `tests\classic_security_bridge_test.c` | 实际加密 flag、live ACL、回调生命周期与失败路径 |
| `tests\test-web-ui.py` | 使用本机 Chromium/Edge 的前端场景验证 |
| `README.md` | 使用、构建、部署观察与明确未验收事项 |

UART 新增 Pico→ESP32 的 `HUB_LINK_OUTPUT=3`，46 字节 payload，每秒一次。
输入 STATE 帧、100 ms 心跳、500 ms 断线释放不变。
电脑状态独立序号、session/nonce 校验，3 秒过期；
遥测不能刷新输入 watchdog。Pico TX 是有界、非阻塞，心跳优先。

SDK 固定 ESP-IDF v5.5.1。加密桥依赖私有 API，CMake 对 `btc_hh.c`
和 `btm_sec.c` 做严格 hash 校验；不要换 SDK 后盲删校验。
所有修改为生成 overlay，没有直接修改 `.deps` 厂商源码。

## 8. 必要命令与操作禁区

```powershell
.\tools\build-esp32.ps1 -BuildDirectory build\esp32-dual-web
.\tools\build.ps1 -DualBluetooth -BuildDirectory build\dual-web -Jobs 4
.\tests\test-dual-bt.ps1
.\tests\test-output.ps1
python .\tests\test-web-ui.py
git --no-pager diff --check
```

这些命令不刷写板子。原可用镜像仍保留于
`build\esp32-dual-bluetooth`、`build\dual-bluetooth`，不要覆盖它们用于“省空间”。

ESP32 Python：
`.tools\esp-idf\python_env\idf5.5_py3.12_env\Scripts\python.exe`。

读取 ESP32 时避免打开端口自动重启：

```python
s = serial.Serial(port=None, baudrate=115200, timeout=0.2)
s.dtr = False
s.rts = False
s.port = "COM12"
with s:
    s.write(b"\r\nstatus\r\n")
```

Pico USB CDC 输出需要 DTR=True；虚拟 USB 上这不等于 ESP32 CH340 的复位行为。
`picotool info -f` **会让 Pico 进入 BOOTSEL 再回来**，
不要为了普通状态查询反复使用；USB `status` 足够。
若必须刷写，先核身份、备份、只写必要区域并 verify；不要现在就做。
Pico 只对 `--ser F1812912A883CCE2` 操作。

Pico UF2 第一块是 RP2350 absolute-family 元数据，flags `0xA000`，
target `0x10FFFF00`，family `0xE48BFF57`，其 count=2 不等于整个文件块数。
程序块 family `0xE48BFF59` 使用自己的编号/count。
验证时不能要求跨 family 全文件统一编号，也不能任意跳过越界块。

关键本地备份（可能含密钥，必须保持 ignored）：

```text
backups\esp32-nvs-before-classic-encrypt-fix-20261001-113932.bin
backups\esp32-nvs-before-softap-20261001-102040.bin
backups\pico-f1812912a883cce2-before-softap-20261001-102247.bin
backups\pico-f1812912a883cce2-after-softap-20261001-102247.bin
```

前两者 24,576 字节，后两者各 4 MB。ESP32 配对区为 `0x9000/0x6000`。
**不要为恢复程序直接覆盖整块 Flash 或恢复整份 NVS**，会改变当前配对数据。

## 9. 下一 session 最小行动

1. 读取本文，确认同一目录的未提交代码还在；只读核对 Git，不刷写。
2. 确认用户当前键鼠仍可用。让管理手机连接热点、打开网页；
   先验证网络连接，避免把客户端没连热点误判成服务坏了。
3. 有界确认最新版本两输入 ready/encrypted、报告增长、PC1 ready、
   UART fresh，以及用户在网页打开时的实际手感。
   若已经满足要求，记录结果即可结束，不做额外实验。
4. 如果确实重现异常，先分清网络、HID 描述符、实际加密、UART freshness
   哪一层失败。保留密钥；不要用重配或免加密替代定位。
   若仅开热点客户端就影响输入，对比 `web off` 后是否恢复，并明确 C1 限制。
5. 稳定后再由用户决定是否提交/推送本轮代码。没有授权不要自动 commit/push。

用户期待的是一个轻量状态网站，不是继续扩展配对控制台或进行无限期蓝牙实验。
