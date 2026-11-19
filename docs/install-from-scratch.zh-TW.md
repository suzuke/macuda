# macuda 從零安裝到可以使用（繁體中文）

適用版本：`suzuke/macuda` 的 main（2026-11-19 之後，含 M1–M7 的 DART 防護、M4 watchdog、伺服器包裝、
tinygrad pkill sentinel、可搬移的 release 路徑）。英文的完整說明在 [README](../README.md)；本文只走一條
從空機器到第一次推論成功的路，並標出每一步做了什麼、失敗時看哪裡。

## 0. 先準備好的東西

| 項目 | 需求 |
|---|---|
| Mac | Apple Silicon（macOS 26 以上最省事：可用 Apple 自己的 `container`） |
| 顯示卡 | RTX 3060（sm_86）、RTX 4090（sm_89）或 RTX 5090（sm_120a），裝在 Thunderbolt 外接盒 |
| 電源 | 外接盒要有**自己的**電源；筆電也接自己的變壓器，不要吃外接盒供電（會在重負載時重新列舉） |
| 磁碟 | 編譯約 25 GB，再加模型 |
| 網路 | 安裝期間要下載：Homebrew 套件、CUDA 容器 image（約 4 GB，一次）、NVIDIA 標頭與 GSP 韌體、模型 |

不需要 NVIDIA 驅動、不需要 Linux 機器：CUDA 的裝置碼由容器裡的 nvcc 編譯（只編譯，不執行，所以容器不需要 GPU）。

## 1. 取得原始碼

```sh
git clone https://github.com/suzuke/macuda
cd macuda
```

（上游是 `Davinchy/macuda`；本文描述的防護都在 `suzuke/macuda`。）

## 2. 檢查缺什麼（不會動任何東西）

```sh
sh install.sh check
```

它逐項報告：Xcode 命令列工具、Homebrew、cmake、python3、Homebrew LLVM（`/opt/homebrew/opt/llvm`）、
可連結的 macOS SDK、容器執行環境（Apple `container` 或 Docker）、TinyGPU.app 與它的 DriverKit 擴充。
`MISSING` 那幾行會附上要執行的指令。Xcode 命令列工具和 Homebrew 要自己先裝（`xcode-select --install`、
https://brew.sh），其餘下一步可以代裝。

## 3. 安裝缺的東西並編譯

```sh
sh install.sh          # 每裝一樣都會先問；YES=1 sh install.sh 則全部同意
```

它會依序：

1. **裝缺的套件**：cmake、llvm、容器執行環境，以及固定版本的 TinyGPU.app
   （tinygpu_releases `c0d024f9`）。
2. **建立 tinygrad 的 pkill sentinel**：只要 TinyGPU.app 在，就在 tinygrad 可能用到的每個 cache 目錄放一個空檔，
   防止 tinygrad 的 `ensure_app()` 執行 `pkill -f TinyGPU` 殺掉正在服務的伺服器（那會讓 bus master 開著時 session
   消失，是 2026-11-18 panic 的成因之一）。
3. **偵測卡的架構**：從 IOKit 讀卡的 PCI id（唯讀）。3060 → `sm_86`、4090 → `sm_89`、5090 → `sm_120a`。
   沒接卡時預設 `sm_120a`，可用 `ARCH=sm_86 sh install.sh build` 指定。
4. **編譯六個階段**：標頭與韌體 → llama.cpp → stable-diffusion.cpp → shim（libtinynv、libtinycudart、
   libtinycublas、緊急 helper `tinynv-bmclear`）→ ggml 的 CUDA 後端（nvcc 在容器裡）→ 連結。
   llama.cpp 約 5 分鐘、CUDA 部分 3–5 分鐘，第一次另加 4 GB 的 image 下載。

執行檔位置：`sm_120a` 在 `cuda-shim/build/bin`，其他架構在 `cuda-shim/build/bin-<arch>`
（例如 3060 是 `cuda-shim/build/bin-sm_86`）。`nv_shim_step.sh` 和 `serve.sh` 沒指定 `BIN` 時，會依連接的卡
（或你 export 的 `ARCH`）挑對應的目錄；沒有為這張卡編過的執行檔時直接報錯，不會拿別的架構的來跑。

**使用容器時**記得先讓它跑起來：`container system start`，或打開 Docker Desktop。

## 4. 核准 TinyGPU 的系統擴充（只能你自己做）

```sh
/Applications/TinyGPU.app/Contents/MacOS/TinyGPU install
```

到「系統設定 → 一般 → 登入項目與延伸功能 → 驅動程式延伸功能」核准。不要用
`systemextensionsctl reset` 當作修復捷徑。

## 5. 起飛前檢查

```sh
sh tools/preflight.sh
```

唯讀，不開啟 GPU。必須看到 `VERDICT: OK` 才往下走。它檢查卡在不在、連結狀態、DART 錯誤旗標、
電源設定，以及「tinygrad pkill guard」（sentinel 是否齊全；缺的話只警告，不擋）。

## 6. 下載模型

用 `hf` 指令放進 `models/`。`install.sh` 不會裝它，先自己裝：`python3 -m pip install --user huggingface_hub`
（或 `brew install huggingface-cli`）。依顯示記憶體挑：

```sh
# RTX 3060（12 GB）：7B/8B 級
hf download bartowski/Meta-Llama-3.1-8B-Instruct-GGUF Meta-Llama-3.1-8B-Instruct-Q8_0.gguf --local-dir models/

# RTX 4090 / 5090：README 的驗證模型
hf download unsloth/Qwen3.8-27B-GGUF Qwen3.8-27B-UD-Q4_K_M.gguf MTP/mtp-Qwen3.8-27B-Q4_0.gguf --local-dir models/
```

模型也可以 symlink 到外接硬碟，讀取速度會影響載入時間。

## 7. 第一次跑

所有碰卡的動作都透過 `tools/nv_shim_step.sh`：它會跑 preflight → 確保 TinyGPU 伺服器在跑
（經過 `tools/tinygpu-server.sh` 的包裝）→ 執行**一個** GPU 行程並記錄 log →（`QUIESCE=1` 時）冷停止。
它**不會**上鎖（GPU 鎖在 2026-09-17 拿掉了），所以不要同時開兩個；`serve.sh` 則會在服務期間持有鎖。

```sh
sh tools/nv_shim_step.sh A opverify                                    # 算子驗證
sh tools/nv_shim_step.sh A simple models/<模型>.gguf 96 '說明 GPU 記憶體頻寬。'
sh tools/nv_shim_step.sh A bench  models/<模型>.gguf 128               # 解碼速度
```

RTX 3060（Ampere）在 Thunderbolt 外接盒上的既定程序是每一步加 `QUIESCE=1`，步驟結束後再做一次冷停止
（FLR、關 bus master）。驅動本身已經會讓 Ampere 卡冷停，所以 `QUIESCE=1` 是額外保險；它需要第 10 節的
tinygrad 復原環境，沒裝的話那一步會報錯，但不影響推論本身。

同一時間只能有一個行程碰卡；兩個行程同時開卡會把卡卡死。

## 8. 提供 OpenAI 相容 API

```sh
MTP=models/MTP/mtp-Qwen3.8-27B-Q4_0.gguf sh tools/serve.sh start   # 3060 用 8B 模型：MTP= sh tools/serve.sh start models/<模型>.gguf
sh tools/serve.sh status
sh tools/serve.sh test
sh tools/serve.sh stop
```

API 在 `http://127.0.0.1:8090/v1`。

## 9. 這個版本內建的安全網（不用設定，預設開啟）

這些都是為了同一件事：**TinyGPU 伺服器的 session 在卡還開著 bus master 時消失，卡會繼續 DMA 到已解除
映射的位址，DART 錯誤風暴最後讓 macOS panic。**

| 機制 | 做什麼 |
|---|---|
| 伺服器遺失偵測（M1/M2/M7） | 用戶端發現伺服器斷了就鎖定「已遺失」，不再送一般指令；收尾時若連線還在，會先關掉 bus master 再結束 |
| 緊急清除（M3） | 用戶端自己偵測到伺服器遺失時，立刻啟動 `tinynv-bmclear`，透過新的伺服器連線把 bus master 關掉。實測約 0.2 秒 |
| watchdog（M4） | 每個 session 旁邊跑一個 `tinynv-bmclear --watch`，負責 M3 管不到的情況（用戶端自己被 SIGKILL、崩潰）。已通過四輪無卡驗證，**尚未上卡做故障測試**；重試間隔最長可達數十秒，時間不如 M3 確定 |
| 伺服器包裝（M6） | 伺服器經 `tools/tinygpu-server.sh` 啟動，記錄退出狀態，並在退出當下與 5 秒後各拍一張 DART 快照 |
| pkill sentinel（M5） | 擋住 tinygrad `ensure_app()` 的 `pkill -f TinyGPU` |
| SIGPIPE 防護 | 伺服器斷線時用戶端不會被 SIGPIPE 直接殺掉，能走完收尾 |

數字：2026-11-18 的真實事故沒有任何防護，32 秒內 129 次 DART 錯誤後 panic。2026-11-19 刻意在推論中 SIGKILL
伺服器：第一輪用戶端被 SIGPIPE 殺掉、M3 沒機會動作，錯誤一直持續到手動清除 bus master 的那一刻才停
（約 11 秒、129 次，沒有 panic）；修好 SIGPIPE 後第二輪由 M3 處理，0.2 秒內 6 次。

紀錄在哪裡：伺服器 log 與 DART 快照在 `logs/`（安裝目錄不可寫時改到 `~/Library/Logs/macuda`），每一步的
log 是 `logs/shim-<步驟>-<時間>.log`。

除錯用的開關（平常不要動）：`TINYNV_WATCHDOG=0` 關 watchdog、`TINYNV_EMERGENCY_BM=0` 關緊急清除。

**仍然存在的風險**：伺服器死掉到 bus master 被關掉之間，最好的情況（M3）仍有約 0.2 秒的錯誤空窗；只靠 M4 的
情況還沒在卡上測過。根治要 TinyGPU 的 dext 在 session 收尾時先關 bus master（給上游的回報還在草稿階段，尚未送出）。

## 10. （選用）tinygrad 復原工具

`tools/nv_quiesce.sh`、`nv_e3_flr.py`、`nv_temp.py` 等復原工具需要舊版 tinygrad（上游已在 2026-09-05 移除
macOS 遠端 PCI 路徑）。本機的做法：

```sh
git clone https://github.com/Davinchy/tinygrad tinygrad-stable
git -C tinygrad-stable checkout 33cd373ad
python3.12 -m venv venv      # 沒有 python3.12：brew install python@3.12
. ./env.sh          # 設定 EGPU_ROOT、PYTHONPATH（指向 tinygrad-stable）、venv、XDG_CACHE_HOME
sh tools/tinygpu-sentinel.sh create   # 新的 tinygrad checkout 可能固定別的 TinyGPU 版本，補齊它的 sentinel
```

不要把 `APL_REMOTE_SOCK` 設成全域環境變數。

## 11. 出事時

- **卡不見、`pci-dart-error-data` 旗標被設、連結檢查失敗**：關掉外接盒電源、重插，再跑 `sh tools/preflight.sh`。
  沒有任何腳本能代替實體重插。
- **重插或重開機之後 preflight 仍然 ABORT**，說 dext 重新列舉過或 DART 旗標還在：確認卡已經重插、狀態正常後，
  由人執行 `sh tools/dart-stale.sh --clear`（忘掉舊紀錄，下一次乾淨的 preflight 會重新記錄），再跑一次 preflight。
- **不要**手動 `pkill` TinyGPU 伺服器、不要 `systemextensionsctl reset`。
- 看 `logs/tinygpu-server.log` 的退出狀態行和同時間的 `dart-at-server-exit-*.txt` 快照。

## 12. （選用）打包成可搬移的 release

```sh
ARCH=sm_86 PREFIX=/絕對路徑/macuda-release sh setup.sh stage
```

會複製該架構的執行檔、`tinynv-bmclear`、GSP 韌體和 `tinygpu-server.sh`，排成
`bin/`、`libexec/macuda/`、`share/macuda/`。整個目錄搬到任何地方，程式都會在自己旁邊找到 helper 與韌體。
只複製，不打包、不上傳。
