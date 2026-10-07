# ESP32 + Pico 2 W 热点网页交接

更新日期：2026-10-07。最新状态见首节；下方早期日期/用户确认属于历史记录。

## 2026-10-07 20:28 用户要求保存当前可用release（最新）

用户先反馈“鼠标又好了，不再频繁断”，随后明确要求commit为release。
本轮只保存代码/本地tag，**不刷板、不重启、不改变参数/配对/选中电脑**。
release名称`v0.6.11`，实际组合ESP32 `0.6.11-wifi-default-off` +
Pico `0.6.2-hid-dedup`（Pico不变），现有v0.6.3等标签不动，不push。
用户确认对应当前版本的可用状态，不等于已经找到鼠标超时根因。

关键是release不能混入未上板debug：先将工作区未部署的diag相关source/
tests备份到本机会话目录
`pending-debug-before-v0.6.11-release`，所有原pending全diff备份
`pre-v0.6.11-full-working.patch`；随后只撤回本会话新增日志candidate，
保留用户已部署sleepOFF/mouseactivity/WiFi默认OFF等改动，调整测试提取
目标为实际release。不修改Git HEAD/分支、不stash/reset/rebase、不删除
配对数据。**当前源码已不含diag候选，basic status/history保持**；
以前“当前工作区版本xxx-diag”的段落都是历史，日志代码仍可从backup恢复。

逐文件核对当前tracked `esp32/include/src` 87份文件与实际部署隔离source
一致（仅规范化CRLF）；Pico source/构建版本相对ffc7f38不变。最新已部署
镜像仍74CCD804（ESP）/2640C219（Pico），build目录没有覆盖已部署镜像。
从repository根source用部署sdkconfig重新完整构建在
`build\esp32-release-v0.6.11`，不刷入。路径/编译元数据不同允许hash不同，
不要说release rebuild必定与隔离部署binary逐字节复现。
重新构建通过，无编译warning，镜像SHA256
`7596631D133C2AF38F0838CDBB998EAC0C8000E7B9EE77106E9546B3629CCD70`，
1452128字节、app partition剩83872字节；actual板上仍74CCD804镜像，
本轮未刷入rebuild。staged87份firmware文件同样与部署source一致。
release生产mouseactivity/cleanretry/records/webstartup/button回归和原
33组output/dual去重通过。异常使用HCI0x08发生过、随后无频繁断线只是
用户观测改善；release notes保留这个限度，不能承诺永久稳定/4PC验收。

## 2026-10-07 默认Wi-Fi关闭与使用中鼠标断线监听

用户要求核对WiFi并刷入“默认关闭，只有双击开启”；随后要求先监听使用中
也掉线。首先只读确认原0.6.10当时WebOFF，因此不能再说是WiFi未关导致。
暂停刷写，42秒被动监听（前35秒无status/history输出）捕获：
`BT_HCI rsn0x8`在26382610ms，应用cleanup26382623ms，CLOSE reason8；
底层先超时、13ms后应用才收尾，无新的activity参数请求/协商错误记录。
键盘/PC1持续在线，UART framing0、输出queue0/overflow0。
鼠标恢复后报告259023，后续又读到ready仅5秒、reports259817→259872，
仍是7.5ms/latency44/timeout2160，attempts0/invalid0。这与用户使用中
也断线的反馈一致，不得说一定是省电、WiFi或大厂鼠标不可能出问题。
确切底层原因仍未定位；此前关闭WiFi一次稳定只是有限观察，不是根因证明。
原始日志留session files `mouse-active-drops-20261007-*.log`，
不记录按键/不上传私密数据。Pico历史最大queue等待6.6秒不是当前队列
积压，不当成这次根因（当前queue0/普通报告仍增加）。

继续完成用户已授权的单点WiFi改动，实际已部署：
**ESP32 `0.6.11-wifi-default-off` / Pico `0.6.2-hid-dedup`不变**。
boot只初始化WiFi配置，不 `esp_wifi_start`/HTTPstart；每次重启仍OFF，
不持久ONflag。BOOTruntime double-click是唯一开启路径，长按仅录入，不
隐式开WiFi；USB `web on`明确拒绝并提示doubleclick，保留web status/off/
password，密码写入不会把OFF热点开启。要网页管理先双击开热点再长按授权。
同一点击规则仍可双击关闭。仅这些路径改动，其余sleepOFF/mouseactivity/
latency/timeout/重连/parse/records/Pico/SDK完全原样；不混pendingdiag。

独立source来自本轮前实际0.6.10代码快照，逐文件核对只有main控制按键/
help、hub_web初始不开/USBon拒绝、版本CMake/header四文件预期差异。
真实hub_web_init/set_enabled/command函数提取测试bootOFF、物理路径可
toggleON/OFF、on拒绝、status/password不偷偷启用、initialization/控制
错误保留；真实按钮helper测试long/short/none不enable、long仍enrollment，
double才返回toggle。原输入/解析/cleanup/鼠标activity安全和Picooutput
回归通过。一次stub `esp_err_to_name`静态声明warning导致-Werror失败，
仅改stub符合SDK声明后通过，不放宽warning或生产行为。最终build无warning。

```text
build\esp32-bt-awake\pico_dual_bt_input.bin
SHA256 74CCD804E2401589B399D8A26072529E3EC198E37DB18863663447664B055520
backups\esp32-0.6.10-before-wifi-default-off.bin
旧镜像SHA256 42E908C868C855AD8E2056013089C3BAA9C265FD5181F4C128E74932B2049798
```

核MAC10:52:1C:1D:92:34，COM15 app-only0x10000写入1452160字节verify。
最新NVS0x9000/0x6000启动前字节保持不变（不回灌旧keys/password）：
`backups\esp32-nvs-before-wifi-default-off-20261007.bin`、
`backups\esp32-nvs-after-wifi-default-off-before-boot-20261007.bin`。
postboot实际WebOFF/initESP_OK/firmware0.6.11，鼠标encrypted/HIDready、
11.25ms/latency44/timeout2160、0 reports：未做真实移动测试，peer参数
保持原策略不因ready强制7.5ms；键盘当时仍page/ESP_FAIL未ready。
Pico0.6.2一直没刷/重启，PC1ready/queue0/overflow0、UARTonline/framing0。
**这只验收WiFi默认关闭，不是频繁鼠标断线已解决**。没有为恢复键盘额外
重启、重配或改参数；等设备可达/活动再验收真实转发，不注入模拟输入。

当前工作区版本0.6.11-wifi-default-off-diag仍含之前未上板diagnostic，
板上是不含diag的0.6.11-wifi-default-off。HEADffc7f38不变，无commit/push。
本轮前部署source压缩备份/session `mouse-activity-isolated-before-wifi-default-off-20261007.zip`；
工作区原pending diff也备份。不能用旧verifymouse脚本代替新WiFi精确比对，
本轮脚本`verify-wifi-default-off-source.py`。

## 2026-10-06 09:30 用户要求活动时调间隔，并授权直接刷板（最新）

实际已刷 **ESP32 0.6.10-mouse-activity**，Pico仍0.6.2，没动Pico；
不混入待上板diag代码，蓝牙modem sleep保持OFF。只改变鼠标参数触发：
HID就绪、BLEauth成功和参数更新回执均不自动请求7.5ms；真实移动/滚轮/
按钮变化才考虑请求，keyboard或零动作重复状态不算mouse activity。
队列先保存报告再请求参数，不等待协商才转发；间隔/timeout/latency值
由SDK读取，**只请求interval=7.5ms，latency和timeout原样保留**。
SDK读取和请求至少间隔10秒、pending时不重发，连续最多3次未满足快速
间隔就不再干预，确认快速间隔或重连才重置预算。没有idle定时主动关闭
或强制降速；第一下仍受省电时实际间隔影响，不承诺零延迟唤醒。

隔离source继续从ffc7f38导出基线＋sleepoff＋本次activity策略构建，
policy/helper与生产callback测试的工作区内容逐字节一致，所有其他导出
文件逐一核对。未混入此前withdrawn优化或未提交diag，不改SDK/Pico/NVS/
重连/解析器/监督超时。旧0.6.9镜像先备份，复用build目录快速增量build：

```text
build\esp32-bt-awake\pico_dual_bt_input.bin
SHA256 42E908C868C855AD8E2056013089C3BAA9C265FD5181F4C128E74932B2049798
backups\esp32-0.6.9-bt-awake-before-mouse-activity.bin
旧镜像SHA256 BA06B40B666A731B489DA36B457FA52E96FD4E4B696E5320195320E7F6975A59
```

原native输入/录入/生命周期、33组Picooutput/dedup回归通过；真实
hid_event+实际参数policy测试idle/peerupdate无请求、motion/wheel/button
触发、1000数据包pending期间正常转发、10秒边界、连续失败限额、已快/
读取失败/安全门禁/配对试用隔离/重连reset/wrap。build无warning且sleepoff。
测试中报告计数初次断言错1，按实际1keyboard+1noop+1motion+1000修正，
生产逻辑未为通过断言偷改；隔离source初次比对空白缩进差异已统一核验。

COM15核MAC10:52:1C:1D:92:34，app-only0x10000写入1451952字节并verify。
当前配对/设置NVS启动前逐字节不变：
`backups\esp32-nvs-before-mouse-activity-20261006-0931.bin`、
`backups\esp32-nvs-after-mouse-activity-before-boot-20261006-0931.bin`。
Pico0.6.2串口确认UART online/framing0、PC1ready/queue0/overflow0，
当时mouse role尚未恢复，没有实际鼠标数据验收。ESPconsole刷后多次出现
COM15 mapping unavailable/FileNotFound/WriteFile AccessDenied；一次只读
PnP/DOS mapping显示返回后再次打开write仍失效。没有为修USB乱重启或
强行重枚举，已停止反复串口探测。**最终协商参数和掉线改善未读到**，
不能把flashverify说成用户手感已通过。

09:05用户已双击关WiFi，WebOFF实测。但app重启会默认start热点；
尝试query web时写失败，未确认/恢复OFF；用户若要继续保持关闭可双击
ESPBOOT。这个USB限制需如实交代，不能声称热点仍关闭。无commit/push。
当前工作区含pendingdiag版本0.6.10-mouse-activity-diag，板上是无diag
0.6.10-mouse-activity，不要从当前工作区label误认已部署内容。

## 2026-10-04 23:28 用户要求关闭ESP32蓝牙sleep

用户明确“不需要考虑省电”，仅关闭ESP32蓝牙controller modem sleep。
查当前0.6.7实际sdkconfig发现默认ORIG modem sleep开启、主晶振计时，
CPU PM关闭；旧版同样开启，因此不是已确定root cause。之前鼠标真实
HCI0x08断线、空闲相关但并非每次空闲必断，不把常见BLE省电说成正常
反复断线，也不先改鼠标latency来代替验证ESP自身sleep。

本轮配置 `CONFIG_BTDM_CTRL_MODEM_SLEEP=n`，编译时整个controller
sleep模式NONE，而不是调用不支持接口后忽略错误。实际SDK在modem sleep
未启用时使用 `BTDM_MODEM_SLEEP_MODE_NONE`；CPU PM仍关闭。主工程源码
加同配置与开机提示，保留未刷入的debug代码、版本`0.6.9-bt-awake-diag`。
**板上实验镜像不混入diag**：由 `git archive ffc7f38` 导出到ignored
`build\esp32-bt-awake-source`，只改sleep默认、开机说明和版本0.6.9，
`.deps` junction复用固定SDK而不改SDK。实际build目录`build\esp32-bt-awake`。
所有导出文件与accepted zip逐个比对（仅规范化CRLF），确认只有4个文件
精确预期差异；鼠标7.5ms/latency44/timeout、retry、NVS/网页/扫描、Pico
均accepted基线不变。不撤回未提交日志、不commit、不push、旧镜像不覆盖。
实际generated sdkconfig/header已验证modem sleep OFF、PM OFF；不能仅
凭defaults宣称旧构建目录里sleep也关了。
已完成独立build，无warning；用户授权下只写ESP32application，实际
板上**0.6.9-bt-awake**，Pico仍0.6.2，未刷/重启Pico。日志候选未混入。

```text
build\esp32-bt-awake\pico_dual_bt_input.bin
SHA256 BA06B40B666A731B489DA36B457FA52E96FD4E4B696E5320195320E7F6975A59
```

MAC核`10:52:1C:1D:92:34`、COM15，app-only0x10000写入1451664字节、
verify匹配，当前NVS0x9000/0x6000启动前逐字节相同：
`backups\esp32-nvs-before-bt-awake-20261004-2329.bin`、
`backups\esp32-nvs-after-bt-awake-before-boot-20261004-2329.bin`。
没有删配对/密钥或覆盖原NVS；旧0.6.7可用镜像仍D8F1393D，HEAD仍ffc7f38，
未commit/push/改tag。导出代码可以用accepted zip+精确4文件差异复现。

38秒启动读取确认版本0.6.9，键盘和鼠标正常加密/HIDready，keyboard11
报告、mouse391报告、invalid0，鼠标实际7.5ms/latency44/timeout2160ms，
PC1 ready/queue0/overflow0，Pico0.6.2/UART framing0。没有注入输入。
此窗口未看到鼠标CLOSE，但不是人工控制的移动/静置重复试验，不宣称根因
确定或所有空闲掉线已消除。开机sleep日志可能在串口接入前已经输出；
生成config/header与最终binary检查保证只编译sleep-disabled路径，
没有伪称捕获过开机提示。原生input/生产diag回归也通过。
新的工作区0.6.9-bt-awake-diag包含未上板日志，不等于实际0.6.9-bt-awake；
既有build目录需重新配置n才能继承，默认修改不覆盖历史sdkconfig。

## 2026-10-04 16:08 已提交可用版本；日志候选单独开发

可用0.6.7版本已按用户要求提交 **`ffc7f38`**，没有push/改标签。
当前板上仍ESP32 `0.6.7-clean-retry` / Pico `0.6.2-hid-dedup`，
用户反馈暂时未发现问题。后续仅debug日志候选 `0.6.8-connection-diag`，
**未刷板、未commit**，独立构建目录 `build\esp32-connection-diag`；
不覆盖可用0.6.7镜像，不重启设备，不修改连接/重试策略和外围功能。

新增USB只读 `diag`：每input连接attempt ID、原始失败code/domain/time、
HCI常见错误说明、last descriptor SDK read seen/status/maps/data-present/
length/CRC、当前dev/ACL/worker/recovering/paused和retry delay、事件计数、
report age、UART framing/overflow。`history`增强记录attempt elapsed、
generation、Classic handle、资源状态bitmask，仍最多24条RAM事件。
失败历史独立保留，不被ready掩盖；紧随ACL失败的通用ESP_FAIL不覆盖具体
HCI status。只分析HCI域，其他SDK错误不错误套用HCI意义。
不同失败回调可以多计数，计数不是“独立故障次数”；按target配置重置，
环形覆盖不清累计计数，重启不持久。初次incoming ACL与补HID保持同轮ID，
cleanup完后新连接才新ID，elapsed包含ready期间时间，非物理端到端时延。

重要：`diag/history`先短持锁复制，**解锁后打印**，新增记录路径仅写RAM，
不每个按键/鼠标包输出、不额外广播/查SDK/写Flash；不会查日志时主动
重连。既有 `status` 锁内打印、NVS并发、mouse参数、Pico队列/切换、网页
轮询JSON保持不变。本轮没有重引入此前撤回的优化。
只记录连接元数据，不记录按键/按钮/位移、PIN/passkey、密钥或密码。
既有配对交互提示和web-password USB命令不属于这份redacted输出。
网页仍显示基本事件；新增完整字段走USB，避免增加JSON内存/轮询负担。
资源状态/cleanup结果/stale详情码解读见README。
最终生产callback/记录/打印测试通过，验证同轮ID、错误域优先级、时间回绕、
计数保留、descriptor摘要以及diag执行前后连接/输入状态不变、每次printf
均未持input mutex。原输入录入/CRC/释放与33组Pico输出/dual去重回归通过。
ESP32独立build通过，无编译warning，未刷板：

```text
build\esp32-connection-diag\pico_dual_bt_input.bin
SHA256 1E3C748FA8E3851C801C6AAB6B5C846917C27108ABE7F7A51D63F625FFFEA47B
```

当前板上0.6.7镜像仍是D8F1393D，未重启或修改记录；新diag命令只有更新
日志候选后才存在，不能现在在0.6.7上声称已使用。日志补丁留未提交，
ffc7f38始终指用户确认的无新增日志版本。debug性能实机观察未宣称验收。

## 2026-10-04 16:08 用户确认初步可用并要求提交

用户反馈0.6.7“暂时没发现有什么问题”，要求先commit当前版本，再单独
增加debug日志。此次认可对应ESP32 `0.6.7-clean-retry` / Pico `0.6.2-hid-dedup`，
不扩展为所有故障路径或长期无线验收；不改tag、不push。后续日志仅连接
元数据，不含按键内容、PIN或密钥，不改已确认可用的连接/转发策略。

## 2026-10-04 15:35 用户授权刷入单点重连补丁

用户明确要求“把代码刷进去我看看”。仅更新ESP32，Pico不刷、不重启，
仍运行 `0.6.2-hid-dedup`。本次实际ESP32运行 `0.6.7-clean-retry`：

```text
build\esp32-clean-retry\pico_dual_bt_input.bin
SHA256 D8F1393DCFCD879C232FF88F71AC311DE1A4D0D64C0A63A1ECD4EA08A5D51869
```

COM15核MAC `10:52:1C:1D:92:34`，仅写 `0x10000` application，
1452304字节verify成功；最新NVS `0x9000/0x6000` 启动前逐字节保持不变：
`backups\esp32-nvs-before-clean-retry-20261004-1535.bin`、
`backups\esp32-nvs-after-clean-retry-before-boot-20261004-1535.bin`。
旧三条输入记录、键盘bond和本机热点设置保留，不恢复旧NVS覆盖当前数据。
可用0.6.5回退镜像仍留备份，未commit/push/改tag。

运行时已读回ESP32 0.6.7与Pico0.6.2。PC1 encrypted/HID-ready、
queue0/overflow0/UART framing0，键盘尚未回应page（HCI0x04），两输入
启动采样时仍在连接、无真实报告；键盘失败日志显示cleanup后下一次
OPEN，paused0，没有再把普通失败锁成unsupported。这只证明固件启动、
数据保留与普通重试路径，不代表用户已经确认按键无延迟/重复。等用户
保持原Hub通道唤醒键鼠后验收，不注入输入、不重配、不追加其他改动。

## 2026-10-04 15:06 单点清理重试补丁（历史，当时未刷板）

用户同意仅实现“保留配对和密钥→失败退出→清理完成→重新连接”。
板上保持已确认可用 **ESP32 0.6.5 + Pico 0.6.2**，没有在本轮刷写、
重启、重配、删记录或切电脑。候选源码标记ESP32 `0.6.7-clean-retry`。
用户不可响应是否现在更新/验收的问题，因此不再先部署后等用户试手感。

之前撤回的未提交0.6.6/0.6.3候选代码已备份到本机会话目录：
`withdrawn-candidate-before-scoped-retry-20261004.patch`，
单独新增test也已备份；只撤回本会话自己写的候选，不改HEAD、分支、
用户配对、交接历史。当前源码从 `b401938` 基线起仅重连补丁；
Pico代码、鼠标参数、NVS并发、扫描/按钮/网页策略、UART格式全部基线不变。
没有stash/reset/rebase/commit/push/修改tag。

普通HID OPEN失败、SDK返回NULL、HID CLOSE统一走完整cleanup，原ACL也关闭，
不再保留失败连接补HID。SDK worker/设备/ACL任一还占资源，不允许排下一次；
真正全部结束后用既有1/2/4/8/15秒backoff重连原地址/密钥，并清HID临时状态。
初次正常incoming ACL还可完成未失败的HID建立；未完成不等于已经失败。
关闭时安全清键鼠状态，经原UART逻辑释放电脑端，未改去重/输出队列。
cleanup截止仍拒绝未确认释放的资源；晚到真实cleanup完成自动解除普通
cleanup暂停并重试，不解除security/无效报告/操作员暂停。
SDK本身无限等待没有被假取消或强行free，卡死仍需操作员处理/重启。

描述符读取失败、maps空、data空、length0等不再立即永久暂停；
这些清理重试且清掉“不兼容”连续计数。nonempty bounded描述符解析/role
失败也先清理重试，**3次独立尝试同长度同实际字节**仍失败才停止；
不只比较CRC、不猜boot格式、不绕过安全/真实坏报告校验。成功/不同数据
清计数，分配失败报错且不当成不兼容。descriptor-retry环形事件和USB日志
记录status/maps/失败map/length/CRC/roles/次数，不记录实际按键或密钥。
临时保存的失败描述符仅RAM，设备换槽/forget/成功验证时释放。

新增测试抽取真实 `hid_event` 和生产lifecycle，覆盖failed OPEN与活ACL竞态、
SDK zero-map OPEN、晚/重复OPEN/CLOSE不能复活旧尝试、worker结束、双方断线
顺序、确切3次拒绝、不同/缺失/成功重置、真实解析器的按下/松开。
保留原输入录入、串口安全和Pico去重/输出回归。两种回调断线顺序确认
后才重开；不把模拟测试说成实际手感验收。
最终原生输入/录入/协议/真实callback测试和原33组Pico输出/dual去重回归
通过，ESP32候选完整build通过且无warning。独立候选镜像：

```text
build\esp32-clean-retry\pico_dual_bt_input.bin
SHA256 D8F1393DCFCD879C232FF88F71AC311DE1A4D0D64C0A63A1ECD4EA08A5D51869
```

这是**未部署**镜像，不能据此更新“实际板上版本”。更新前只读采样仍确认
运行0.6.5，keyboard encrypted/HID-ready、reports7698（采样期无新增）、
mouse9974→10086/7.5ms；用户当时不方便确认按键，未注入输入。
Pico未build/未刷/未重启，0.6.2镜像hash保持原值。实际唤醒、重连和重复
按键/手感验收等用户可参与时再做；当前不再无验收把候选放到板上。

## 2026-10-04 13:57 板上已回退可用基线

用户在13:50报告严重卡顿和大量重复按键。立即通过ESP `disconnect 1`
暂停键盘，Pico `release` 发送全松开；PC1 queue0/release0，原配对未删除。
13:41诊断曾发现 keyboard `paused=1/pause-reason=3/invalid=0`，说明被
描述符校验暂停而非坏报告；一次软件restart仅清暂态，未修根因。
不能把新候选说成已通过真实键鼠验收，也不能把重复按键归咎用户操作。

恢复已验证baseline镜像，不改Git HEAD/源码，不恢复旧配对备份覆盖当前
数据，不擦NVS。用户不可响应回退选择，依据候选实际回归撤回自有板上
更新；不是声称用户已经确认选项。当前板上：

| 板子 | 实际运行固件 | SHA256 |
| --- | --- | --- |
| ESP32 | `0.6.5-reconnect-phases` | `2D0707A5747ABB8AC18EECDDF52243C0B48527F777C14735C893E4E42B475136` |
| Pico | `0.6.2-hid-dedup` | `2640C2197B08C6F8CEBA6F06A460000964C7F39D33E9F6E7134861D5A2D32F95` |

ESP32核MAC，app-only回退并verify，回退当时最新NVS启动前逐字节不变：
`backups\esp32-nvs-before-usable-rollback-20261004.bin`、
`backups\esp32-nvs-after-usable-rollback-before-boot-20261004.bin`。
Pico保存回退前当前完整flash，仅加载基线程序，2090真实UF2块一致、
893非程序扇区不变（密钥/身份保留）：
`backups\pico-before-usable-rollback-20261004.bin`、
`backups\pico-after-usable-rollback-before-boot-20261004.bin`。

13:57串口确认上述两版本，keyboard encrypted/HID-ready、paused0、
真实reports515/invalid0；Pico接到keyboard角色，PC1 queue0/overflow0、
framing0，keyboard队列avg466us/max1288us。鼠标也encrypted/HID-ready。
这些最初是设备侧状态和计数，键盘当时有非中性物理状态，未记录按键内容。
用户随后明确确认“这个版本可用”：这是对回退后0.6.5/0.6.2基线的确认，
不是对0.6.6/0.6.3候选或所有异常恢复路径的认可。继续工作以基线为准，
不要再未获实机验收就部署多项候选；先聚焦FILCO描述符完整性/解析失败点，
不要混入鼠标参数、NVS并发、PC切换策略等扩大改动。

当时HEAD仍为 `b401938`；0.6.6/0.6.3候选源码和未提交修复保留在工作区，
**它们当前不在板上**，不要从build目录最新文件或工作区源码推断已部署。
没有push、改标签、stash/reset/revert代码。后续先补描述符完整性/失败点
诊断与单一缺陷复现，不再批量叠加试验或把原生测试当真实手感验收。

## 2026-10-04 可用基线与后续可靠性修复

用户要求先 commit 当前可用版本，再继续修复。已提交 **`b401938`**：
记录已有 `000b7f8` 代码和0.6.5/0.6.2已部署基线、12:53键盘300→339真实
报告以及PC1 queue0/overflow0。没有改 `v0.6.3` 标签、没有 push。
随后修复留在工作区，**没有把后续候选自动再 commit 为已验收版本**。

最终候选已部署 ESP32 `0.6.6-bounded-recovery`、
Pico `0.6.3-switch-recovery`：

```text
ESP32 build\esp32-slots-buttons\pico_dual_bt_input.bin
SHA256 11A4089F1BDEDFDD2DC0D450183E1E1A583A68A768E1DD438B6784BC07E4871F
Pico build\dual-bounded-recovery\pico_bt_hub.uf2
SHA256 3730B8187D266D345FE149030BF7C766E54B67B89E96DFD663F55A449BDD4B19
```

主要修复：

- `user-code` 阶段保留原120秒配对/整体截止，ACL/CONNECT事件不能把等待
  PIN的状态改回15秒HID阶段；普通连接阶段截止保持不变。
- SDK回调/worker缺失导致清理截止时，不重复open/不强行free。试配进入
  明确phase5故障，不再说“已取消恢复成功”。晚到的真实CLOSE可完成回滚；
  否则USB `restart` 或物理授权后确认重启输入板，保留registry/bonds/热点。
  **SDK内部 portMAX_DELAY 未强行改为可取消；恢复依赖明确的板级重启**，
  不自动重启、不引入循环重启，普通 `retry` 不再谎称能救永久占用的worker。
- 扫描20秒截止，停止再等6秒；丢回调进入radio-state-unknown故障，不能假装
  已停止/再次扫描/删除。STOP成功也推进自动阶段，过时的另一transport
  回调不清掉当前扫描；晚到停止可解除扫描故障。
  可信USB `scan classic|ble` 也走同一manual-management排队/截止路径，
  不再直接启动无active-transport/截止的旁路扫描。
- 双板PC切换释放3秒未成功→请求断旧连接，保留旧bond，**收到HCI断线**
  后才继续。断开被拒或5秒缺确认→明确switch fault，普通输入继续禁止；
  断开被拒可重新选目标，确认缺失需等真实断线或重启Pico，不能假完成。
  UART增加switch-fault标志且校验fault必须伴随switching；网页显示原因。
- USB status/history先复制状态再解锁打印；registry写入/校验在输入锁外
  执行，busy锁住竞争管理操作，revision合并写入期间新学到的名称，避免
  名称丢失或覆盖其他记录。实际flash仍可能有平台级暂停，非零时延承诺。
- 诊断有historical error/source/time/count，ready清当前错误；具体ACL
  错误不被紧随的通用ESP_FAIL覆盖。频繁普通重试事件限频，故障元数据保留。
- ESP32真实鼠标活动时请求latency0，至少10秒间隔，保留timeout，允许
  空闲省电更新；参数查询也限频。Pico本地mouse-active开关明确不支持UART
  输入，不再输出“ON所以ESP鼠标已优化”的错误暗示。

dual native/生产生命周期、原33组output+dual去重+切换失败/ACK/wrap、
普通与Classic latency策略、15种headless网页状态均通过；两板完整build
通过，最终ESP32没有编译warning。验证不是向真实电脑发模拟键鼠事件。

部署核硬件身份、app-only ESP写入/verify，最新NVS启动前逐字节不变：
`backups\esp32-nvs-before-bounded-console-final-20261004.bin`、
`backups\esp32-nvs-after-bounded-console-final-before-boot-20261004.bin`。
Pico最新完整4MiB备份/读回：
`backups\pico-before-bounded-final-20261004.bin`、
`backups\pico-after-bounded-final-before-boot-20261004.bin`。
2089实际UF2程序块一致、893非程序扇区不变。最初比较遇到SDK绝对地址
`0x10FFFF00`占位块；根据SDK `UF2_EXTENSION_RP2_IGNORE_BLOCK=0x9957E304`
确认它明确标为忽略，不是4MiB物理flash写入；排除时验证了精确标记/地址/
family，其余块及非程序区逐字节比较，没有放宽物理flash匹配。

**最终实机验收仍缺输入侧可达性：** 最终两固件版本确认、Windows/Mac两
PC都encrypted/HID-ready、PC1仍选中、UART fresh/framing0/storage0；
但最终ESP键盘无HID、page timeout `0x104`，M585仍在连接，本轮输入0报告。
不能据此说“最终新版本键鼠已经不卡”或说键盘必定关机/丢密钥。
第一候选曾收到keyboard52/mouse702，随后键盘remote close `0x13`、鼠标
timeout `0x08`，没有证明原因；最终镜像并未取得持续真实输入/latency0
协商数据。没有强制重新配对/清记录/切电脑/注入模拟输入。
下一步需用户保持键鼠在Hub对应通道并唤醒，再读真实报告/参数与手感；
Mac持续移动、4-PC并发和实机故障注入仍不能用普通PC就绪代替。
日志/程序备份留本机ignored/session files，不提交原始私密数据。

## 2026-10-04 键盘重连逻辑修复

用户授权修复 review 中的重连问题并 **commit，不 push**。Pico 不改，仍
`0.6.2-hid-dedup`；ESP32 最终已部署 `0.6.5-reconnect-phases`：

```text
build\esp32-slots-buttons\pico_dual_bt_input.bin
SHA256 2D0707A5747ABB8AC18EECDDF52243C0B48527F777C14735C893E4E42B475136
```

HID CLOSE 清掉 `profile_requested`，ACL 仍在时等待1秒即可补 HID；
兼容 HID CLOSE/ACL DOWN 两种顺序，旧 handle 的断线事件不能清掉新连接。
普通管理窗口不再停掉全部未就绪输入；扫描只挡同 transport，BLE 扫描不再
阻止 Classic 键盘重连，扫描排队也只等待同 transport 的旧 worker/cleanup。

清理不是“入队就等于断开”：BTU 回执区分 SENT/LOCK_BUSY/OBSOLETE/ABSENT/
FAILED，以 address、handle、ACL generation、request token 核当前请求。
锁忙100ms后重试，回执超时2秒安全重核，清理本身最多20秒。第一次实机
`retry 1` 发现持应用 mutex 提交会让 BTU 立即执行时反复 LOCK_BUSY，约9秒
后才得到 SENT。因此最终版新增应用提交队列，UART task **unlock 后**
才投递 BTU；原生生产函数测试明确断言持锁不调用 bridge，并测队列满及
BTU post 失败，不以巧合成功作为完成条件。

普通 PAGE40s/HID15s/ENCRYPT12s/overall65s；overall 不随阶段进展重置。
显式配对保留120秒安全窗口。缺 SDK callback/资源仍被持有时清理20秒后
报 paused，绝不假释放、重复 worker 或删 bond；SDK 自身的 `portMAX_DELAY`
没有改成可强行取消，若 worker 永不返回仍需诊断后重启。清理超时也不会
覆盖原安全/无效 HID 暂停原因，从而绕过门禁。

USB `status` 增加 phase/phase-age/attempt-age/report-seen/report-age、
pause reason、cleanup pending/result 和 last error；`history` 给最近24条
RAM 连接事件。只记录连接元数据，不记录按键、报告原文、PIN或密钥。
网页同样有报告年龄/事件列表以及物理授权后的“重连原设备（不重配）”。
USB `retry 1|2` 不需开启管理窗口；它清普通 backoff，先安全关旧资源，
不打开 PIN 窗口，不清记录，不能绕过 security/invalid HID fault。
正常空闲没有报告是合法状态，不会因 report age 自动断线。

全部 dual native/生产 lifecycle、原33组 output/dual dedup、12种浏览器状态
通过，ESP32 application build 通过。MAC核对 `10:52:1C:1D:92:34`，
COM15 app-only `0x10000` 写入/verify成功，最终镜像1451088字节，
app分区剩84912字节；最新 NVS `0x9000/0x6000` 启动前逐字节不变：
`backups\esp32-nvs-before-reconnect-final-20261004.bin`、
`backups\esp32-nvs-after-reconnect-final-before-boot-20261004.bin`。
USB-only热点密码和3条输入记录保留，没有修改标签或推送。

验收范围：第一次运行 FILCO/M585 均 encrypted/HID-ready，键盘9→27报告、
鼠标369→745，Pico PC1 ready/queue0/overflow0/UART framing0。
这轮 retry 真实完成 ACL DOWN→HID CLOSE→incoming ACL→加密/HID恢复；
但暴露了上面已纠正的持锁提交问题，**不能当成最终锁外版的速度验收**。
最终运行0.6.5确认 M585 encrypted/HID-ready（本轮尚无真实鼠标输入），
FILCO持续 HCI `0x04` page timeout，未进入ACL；一次 `retry 1` 清掉15s
backoff，87ms后实际提交 OPEN，没有PIN/新配对，没有pause。不能仅凭
timeout断言键盘休眠/断电/丢密钥，更不能把“最终尚未回连”说成已修好。
最终锁外 cleanup 实机计时和键盘长期稳定性需要键盘可达后补验收。
本机没有连接热点路由，HTTP读取超时；没有强行切换用户 Wi-Fi，
网页展示/按钮经 headless浏览器验证，不能说最终板上 HTTP 已验收。
相关日志留 session files，不提交设备私密原始日志。

## 2026-10-03 20:43 Pico 冗余 HID 报告修复

用户报告切到 Mac 后鼠标逐渐延迟。修复前 live Mac/PC3
`A4:83:E7:80:A9:61` 间隔15ms、latency0，队列曾满64，overflow从43
涨到72，最大通知提交前等待约985ms。源码确认每次UART完整快照都把
未变化键盘/零动作鼠标再入队，键盘插在移动中间阻断合并；这是真实效率
缺陷，但尚不能证明全部Mac卡顿只由它引起。

仅双板输出增加queue-aware去重：键盘比较最新同类待发送报告，
没有则比较最后notify成功状态；鼠标只有零位移/零滚轮/按钮未变才跳过。
相同非零移动、滚轮和down/up都保留，强制安全释放/切换/重连不去重；
错误notify仍保留待发transition，满队列重复状态不产生overflow。
UART完整状态/心跳/freshness不变，single-radio行为不改。
`latency` 增加 `unchanged-skipped keyboard=... mouse=...`。

Pico已刷 `0.6.2-hid-dedup`，源码及UART版本一致；ESP32未动，仍0.6.4。

```text
build\dual-hid-dedup\pico_bt_hub.uf2
SHA256 2640C2197B08C6F8CEBA6F06A460000964C7F39D33E9F6E7134861D5A2D32F95
```

核序列F1812912A883CCE2，完整flash备份/读回：
2090payload块一致、893非程序扇区不变，Windows/Mac keys均保留。
备份 `backups\pico-f1812912a883cce2-before-hid-dedup-20261003-204203.bin`、
`backups\pico-f1812912a883cce2-after-hid-dedup-before-boot-20261003-204203.bin`。

原输出33组和新增dual生产测试通过：
5000完整snapshot、8次入队/1次发送模拟、one-event移动队列、总移动守恒、
重复key/zero-motion、fullqueue、notifyerror retry、switch/release/reconnect/security。
实际PC1采样：skip keyboard884/mouse294，真实key6/mouse596通知，
peak1/overflow0，鼠标MCU队列avg451us；两台PC重连/HIDready，加密正常；
ESP键盘鼠标正常，input7.5ms、UARTfresh/framing-errors0。

**Mac连续移动验收未完成**：最终选中PC1，用户不可响应切Mac约20秒请求。
没有强制改变用户选中电脑、没有生成模拟移动；不要把PC1数据说成Mac改善
或把重启后清零overflow说成解决。下一步用户切PC3后读取两次队列/通知/
overflow/skip增量并观察真实手感。Mac15ms不等于不可用，不要强制反复
重请求interval、增大queue掩盖问题。
用户在21:04要求提交，本次去重修复作为独立后续commit保存；
未推送，原v0.6.3标签和8efa3c2远端不改。

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
