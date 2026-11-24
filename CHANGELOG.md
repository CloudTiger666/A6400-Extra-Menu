# 更新日誌（Changelog）

本文件記錄 **A6400 Extra-Menu** 各版本在 Sony ILCE-6400 / ILCE-6100 實機與主機端的功能演進、效能調校、重大問題修復與驗證紀錄。

---

## [V4.3 SLEM (未釋出)] - 2026-11-24（開發驗證中）

- **雙重曝光穩定性與一鍵取消重拍升級**：
  - 雙重曝光改為固定使用 `1.7MP` 內嵌預覽（`1616×1080`，雙圖合計約 `10 MiB` RAM），徹底解決 `6MP/12MP` 畫質下同時解碼兩張大圖觸發「影像解碼失敗」的問題。
  - 拍完第 1 張進入 `1/2待拍` 時，主選單右上角「套色相冊」自動變為醒目橘底 **「取消雙曝 1/2」**，點擊一鍵捨棄第 1 張並保持雙重曝光模式開啟（可直接重拍第 1 張，不需進細節設定關閉再開）。
- **防剛呼出假死雙重保護（`tid` 自癒 ＋ `ticker_layer` 重啟）**：[`camera/native_film_ui_trial3.c`](camera/native_film_ui_trial3.c) 於 `animate()` 改為在 `cur_tid != ui.tid` 時自動認領新執行緒 `tid` 不中斷迴圈，並於 `visibility(1)` 剛開啟選單時重新確保 `ui.ticker_layer` 維持啟用與 `set_animate(ui.ticker, 1)`，降低系統切換圖層導致選單停留畫面卻無響應的機率。
- **開機包 `gzip` 壓縮瘦身 52.6%**：[`host-tests/build_film_native_boot_package.py`](host-tests/build_film_native_boot_package.py) 改以 `gzip`（level 9、`mtime=0`）壓縮 `/tmp/a6400-film-boot-v15.tar`（`245,760 B` → `116,517 B`，省下 `129,243 B`），[`host-tests/film-runtime-v2-templates/bootstrap-local.sh`](host-tests/film-runtime-v2-templates/bootstrap-local.sh) 改以 BusyBox `tar -xzf` 解壓並保留 `tar -xf` 備援。
- **開機右上角「已就緒」微提示 ＋ 版號升級 `V4.3 SLEM`**：[`camera/native_film_ui_trial3.c`](camera/native_film_ui_trial3.c) 於開機首次掛載進入 UI 時在右上角閃出 1.2 秒綠色提示 `V4.3 SLEM 已就緒`（不影響 `a6400_ui_toast_v1.seq`）。
- **MPR 掃描抗雜訊與慢速大卡 `scan_timeout` 修復**：[`camera/native_capture_probe.c`](camera/native_capture_probe.c) 新增 `timing_trace_line()` 過濾穿插於 `MPR FileIf FFirst` 回應中的 `line=..., time=..., diff=...` 除錯追蹤行；[`host-tests/build_film_capture_daemon.sh`](host-tests/build_film_capture_daemon.sh) 將常駐模式單次目錄總掃描上限 `SCAN_MS` 與開機基準等待由 `45s/60s` 放寬至 `180s`（單筆指令卡死仍維持 `4s` 保護），並於 [`camera/film_capture_daemon.c`](camera/film_capture_daemon.c) `inventory_test_album()` 重設 `t->scan_started`，解決慢速卡／多照片資料夾開機誤觸 `scan_timeout` 退出問題。
- **套色相冊改讀 TEST 目錄 ＋ 16 槽位 RAM 秒開快取與預抓加速（PR #15 + 效能極速優化）**：
  - 套色相冊張數改為 SD 卡 `/A6400/TEST/F` + 7 位數字 + `.JPG` 的檔名索引，最多 1024 筆；超過時首頁顯示 `1024張+`。取電池後重新列出，不再只留本次開機的 16 張記憶體預覽。
  - **16 張 LRU RAM 秒開快取（`/tmp_bt/a6400-pv/c_*.bin`）＋ 智慧背景預抓**：剛拍完轉檔（含雙重曝光）直接寫入 10 MiB tmpfs 快取，開機掃描後自動預熱最新 1 張，瀏覽時自動預抓前後相鄰 4 張，並不重啟 `mpr_monio`／不重複掃描 `TEST` 與 `DCIM`；12MP 大圖預覽改走 `1/8` DC 極速解碼（`~25 ms`），實現 **0 秒秒開與零延遲連翻**。
  - 未命中快取之舊圖使用 `/tmp_bt/a6400-film-job` 臨時掛載 32 MiB 私有 tmpfs 解碼、完成或退出即卸載清除，並檢查 JPEG EOI（連續失敗 3 次顯示「讀取失敗」）。
  - 修正 16:9 液晶 `640×480` 比例，改以 `448×400` 顯示 3:2 照片（左右留黑邊）並依 EXIF 方向自動轉正直拍；修正重進相冊 `album_view_seq` 單調遞增防序號碰撞。
  - 保留 `dexp_mode`、`dexp_seq`、`dexp_pending` 欄位偏移與雙重曝光狀態，互不衝突。

## [V4.2.1 DEXP (hany)] - 2026-11-23（雙重曝光 ＋ MTF2 自動重測）

- **新增「雙重曝光」（濾色 Screen / 變亮 Lighten / 平均 Average）**：[`camera/author_fast_jpeg.h`](camera/author_fast_jpeg.h)、[`camera/film_jpeg.cpp`](camera/film_jpeg.cpp)、[`camera/film_capture_daemon.c`](camera/film_capture_daemon.c) 實作純整數快速混合（12MP 自動上限 6MP 防 OOM），拍第 1 張跳出 `1/2 已拍` Toast，拍第 2 張自動混合並觸發即時預覽小窗與套色相冊。
- **嚴格 LUT 互斥保護**：開啟雙重曝光自動關閉 LUT；雙曝 `1/2` 未拍完時鎖定禁止開啟 LUT（切回「關閉」即可隨時取消）。
- **MTF2 原生 MENU 觸控自動重測**：[`camera/native_film_ui_trial3.c`](camera/native_film_ui_trial3.c) 於進入「細節設定」或點擊「原生MENU觸控」列時自動重測，解決 A6400 `v2.00` 開機慢載入 `CautionConfig.so` 誤判「不支援」問題；`--diag` 新增 `IM_MAPS` 診斷。

## [V4.1 SLEM (hany)] - 2026-11-23（Sony LUT Extra Menu）

> 實機驗收基準：Sony ILCE-6400（FW `Ver. 2.00`）繁體版 `package=dfa7580185a22524`／公開純淨版 `82e0803acccb631b`（簡體版 `12e10d4d18d68e6e`／`dc2e836c204d4180`）

### 新增功能（Added）
- **哈蘇瑞士工業風 2×2 田字格主選單（無自動隱藏）**：
  - [`camera/film_menu.c`](camera/film_menu.c)、[`camera/film_native_view.c`](camera/film_native_view.c)、[`camera/native_film_ui_trial3.c`](camera/native_film_ui_trial3.c) 全新設計 2×2 清晰佈局（左上 `LUT 設定`、右上 `套色相冊`、左下 `快門聲`／`細節設定`、右下 `畫質`）與單頁雙欄 10 款膠片風格清單，取消 20 秒自動隱藏，讓調整參數不再中斷。
- **100% 保留原廠回看相簿（`▶`）＋ 原生零閃屏真彩色預覽（`RGBA4444`）**：
  - 徹底不挾持 Sony 原廠播放鍵；已套色照片改由自製選單右上角「套色相冊」專屬瀏覽（`608×400`），拍完轉檔完成後於右下角自動彈出 `240×160` 即時預覽小窗。
  - [`camera/film_preview_osd.c`](camera/film_preview_osd.c) 與 [`camera/native_film_ui_trial3.c`](camera/native_film_ui_trial3.c) 改採 `GEN_CacheInsImg`＋`CreateImageParam(fmt=7)` 原生硬體圖層直接繪製，完全零閃屏、零色偏。
- **三段畫質新增 `12MP 超清`（`4240×2826`）**：
  - [`camera/author_fast_jpeg.h`](camera/author_fast_jpeg.h) 與 [`camera/film_jpeg.cpp`](camera/film_jpeg.cpp) 新增 `12MP 超清` 檔位（可於 `1.7MP 極速` / `6MP 高清` / `12MP 超清` 之間一鍵切換）。
- **整合原生 MENU 全觸控（MTF2）**：
  - [`camera/mtf2_native_touch.h`](camera/mtf2_native_touch.h) 在既有 Appframework UI 執行緒直接驅動 Sony 原生 `MENU` 全頁觸控、滑動翻頁與彈窗點選，並可於選單內獨立開關（改採模組相對 RVA＋10 組機器碼指紋驗證＋精確 `/CautionConfig.so` 比對，不相容機型自動安全靜默停用）。
- **零冷卻＋長按 Hi+ 高速連拍快門音（`A-A-A-A-A-B-C`）**：
  - 取消快門音效觸發冷卻限制並加入長按連拍重複節拍（`hold=180ms`、`repeat=95ms`），單拍與高速連拍跟手不卡音（公開版預設不含私人 `.pcm` 音檔）。

### 跨韌體相容性強化（Compatibility — Sony A6400 / A6100）
- **動態掃描與只讀 `/usr` ARM32 ELF 校驗**：
  - [`camera/film_native_command.c`](camera/film_native_command.c) 與 [`camera/native_capture_probe.c`](camera/native_capture_probe.c) 保留原定雜湊快速通道，並加入原始 `SYS_fstat64`/`SYS_stat64`＋`/proc/mounts` 唯讀 `/usr` ARM32 ELF 驗證（無額外 GLIBC 符號相依），跨 A6400 / A6100 韌體免重編。
  - [`camera/native_ui_load_guard.c`](camera/native_ui_load_guard.c) 導入三層觸控掛載搜尋（A6400/A6100 快速偏移 → `WidgetSystem::postMouseRelease` 符號鄰域掃描 → 全 `libObj.so` RX 唯一簽章掃描）。
  - [`camera/native_film_ui_trial3.c`](camera/native_film_ui_trial3.c) 加入 `BMP_*` vtable thunk 與 `GEN_CacheInsImg::clearImage` 動態掃描及 `GEN_Label` 字型防禦回退。
  - [`tools/build_autoexec_hook.py`](tools/build_autoexec_hook.py) 與 [`tools/pmca_deploy_v3_clite.py`](tools/pmca_deploy_v3_clite.py) 改為動態 `.text` 區段修補並支援備援宿主函式庫（`libcompassutil.so` / `libInfraNfc.so`）。
  - [`host-tests/film-runtime-v2-templates/runtime-start.sh`](host-tests/film-runtime-v2-templates/runtime-start.sh) 調降執行緒穩定門檻（`174 → 120`）以兼容 A6100 並要求連續 3 秒穩定。

### 問題修復（Fixed）
- **修復回滾原廠後仍殘留舊版選單**：[`tools/pmca_deploy_v3_clite.py`](tools/pmca_deploy_v3_clite.py) 改為徹底清除 `/etc/ld.so.preload` 內的 `a6400-autoexec` 條目與所有 `.pre-v3c-backup` 檔案並做回滾驗證（不再還原升級前的舊版備份）。
- **修復 Windows WSL2 `PREP_UP_FILE failed rc=None` 與 `054C:0336` 重掛逾時**：[`tools/usbipd_watch.cmd`](tools/usbipd_watch.cmd) 與 [`tools/wsl_deploy.cmd`](tools/wsl_deploy.cmd) 停止對已 `Attached` 裝置重複執行 `usbipd bind --force`，新增 18 秒重掛輪詢、Linux `/sys/bus/usb` 可見性確認與既有 WSL 實例自動腳本同步。
- **修復 FAQ PowerShell 語法錯誤與明確規範「必開藍牙＋必開觸控」**：補上 PowerShell `& ` 呼叫運算子，並於安裝器黑視窗與 [`docs/WINDOWS-WSL2-FAQ.md`](docs/WINDOWS-WSL2-FAQ.md) 加入醒目紅色警告提醒開啟相機 `藍牙功能 = 開`（`飛航模式 = 關`，無需配對手機）與 `觸控操作 = 開`。

---

## [V3.9 SSLF] - 2026-11-20（Sony Super LUT FILM）

> 實機驗收基準：`package=7ec416fa937fab67`（`194,560 B <= 196,608 B`）

### 新增功能（Added）
- **免依賴藍牙開機自動掛載 `/tmp_bt` ＋ `/etc/a6400-autoexec.so`**：
  - [`tools/build_autoexec_hook.py`](tools/build_autoexec_hook.py) 與 [`host-tests/film-runtime-v2-templates/bootstrap-local.sh`](host-tests/film-runtime-v2-templates/bootstrap-local.sh) 在 `/tmp_bt` 仍為唯讀根目錄時自動掛載 `size=2048k tmpfs`，建立鎖失敗時於 `/tmp/a6400-hook-err.log` 寫入單行診斷（無須再強迫維持 `Bluetooth 功能 = 開`）。
- **多組自訂快門音切換與 SD 卡安全覆寫**：
  - [`camera/charo_bind.c`](camera/charo_bind.c) 擴充支援三組 `MS_BIND` 可選槽位（`0x33 Dslr_Emnt_WPS_Connect.bin`、`0x34 Dslr_Emnt_WPS_PBC.bin`、`0x35 Dslr_Emnt_WPS_Push.bin`）與 `--apply` 命令。
  - [`camera/native_film_ui_trial3.c`](camera/native_film_ui_trial3.c) 與 [`host-tests/build_ui_trial3.sh`](host-tests/build_ui_trial3.sh) 支援單槽與多槽（`A6400_CHARO_MULTI=1`）循環切換：`日文 Charo` → `Bling Bling`（或 `SD自訂`）→ `iPhone 快門` → `關閉`。
  - [`camera/film_native_command.c`](camera/film_native_command.c) 新增只讀 `IFSYS sound` 命令讀取 SD 卡 `A:/A6400/CHARO.PCM`／`SHUTTER.PCM`（`1–29,760 B`）原地覆寫 `tmpfs`；**明確規範音效必須編碼為 Sony 24 kHz SDS 32-byte ADPCM**（含對應驗證結果：43–50 dB SNR、`bad_hdr=0`）。
- **選單即時狀態顯示（`a6400_ui_status_v1`）**：
  - 在維持 `sizeof(ui) == 1120` 快照 ABI 不變前提下，新增獨立導出結構 `a6400_ui_status_v1`（magic `0x53544156`），於主標題列即時顯示轉檔中／已轉張數與 `/1024` 佇列容量，並於「膠片細節」分頁底部顯示 `佇列:M/1024 | 畫質`。
- **防截斷安全部署器、`--safe-serial`（A6100 / Windows 強化）、`--rollback` 與 `--diag`**：
  - [`tools/pmca_deploy_v3_clite.py`](tools/pmca_deploy_v3_clite.py) 將所有 `serviceshell` 指令分塊控制在 `<= 180` 字元並支援 `extra_persistent_files` 與雙向 SHA-256 讀回校驗，新增 `--rollback`（還原 `.pre-v3c-backup` 或清除掛勾）與 `--diag`（蒐集 10 項機身狀態與日誌）。
  - 納入 Windows / Sony A6100 實測強化：Windows 自動啟用 `--safe-serial`（`100 B / 20 ms` 分片寫入、`fast=False` 收尾 `0.15s` 上限防雜訊卡滿 timeout）、`echo` + `wc -l` 冪等防重複寫入與 `__B64_L__` 行數預檢。

### 效能最佳化（Performance）
- **無 NEON 環境下之位元等價 6MP 解碼倍速優化**：
  - [`vendor/picojpeg.c`](vendor/picojpeg.c) 實作 32-bit MSB 位元緩衝、暫存器快取 AC 解碼與 DC-only EOB 快速通道；[`camera/rt.c`](camera/rt.c) 改為 32-bit 字組 `memset`。
  - 主機基準測試 `A6409675-original.JPG`（`6MP`）：`decode_ms` 由 `249 ms → 138 ms`（全流程 `304 ms → 194 ms`），輸出 JPEG 與 V3.5.1 **100% 位元一致**。
  - 實機冷開機實拍（`A6409868.JPG`，`portra_400`, `grain=2`, `halation=1`, `1.7MP`）：`read=1350ms`、`render=3140ms`（`decode_ms=1166`, `film_ms=496`, `encode_ms=1147`）、`commit=1240ms`，總耗時 **`total_ms=6690`**（`result=0`、`target_verified=1`）。

### 問題修復（Fixed）
- **修復 PMCA 切換服務模式逾時（`USBTimeoutError` / `054C:0CA8 → 054C:0336` 卡死）**：
  - [`camera/film_capture_daemon.c`](camera/film_capture_daemon.c) 在 `FILM_CAPTURE_PERSISTENT` 模式下於 `scan == -2`（`media_unavailable`，含 USB 大容量儲存卸載 SD 卡時）主動呼叫 `transport_stop(&transport)` 釋放 `/usr/bin/mpr_monio.elf` 並休眠 `2000u` ms；實機冷開機後直接插線跑 `--diag` **無須 `/disable_hook` 即一次順利切入 `054C:0336`**。
- **修復 `/etc` 與 `/tmp` 共用 `/dev/nflasha10` Inode 導致誤刪開機掛勾**：
  - A6400 上 `/etc` 與 `/tmp` 同為 `/dev/nflasha10` 目錄別名（同一 inode）；[`tools/pmca_deploy_v3_clite.py`](tools/pmca_deploy_v3_clite.py) 移除在安裝 `/etc/a6400-autoexec.so` 後誤刪 `/tmp/a6400-autoexec.so` 的邏輯並保護回滾路徑。
- **修復 `/tmp_bt` 預設 `1024k tmpfs` 上限空間不足**：
  - 部署暫存改掛載獨立 `size=2048k tmpfs` 於 `/tmp_bt/v3c_up`，部署完成前自動卸載清理。
- **修正守護行程掃描與錯誤恢復**：
  - [`host-tests/build_film_capture_daemon.sh`](host-tests/build_film_capture_daemon.sh) 調整穩定窗口 `CAPTURE_WAIT_MS=4000u`、`CAPTURE_BASELINE_INTERVAL_MS=750u`、`CAPTURE_STABLE_MS=800u`。
  - [`camera/native_capture_probe.c`](camera/native_capture_probe.c) 安全略過非錯誤類 `[AVIMMGR]`／`[MprCnv]` 背景雜訊；[`camera/film_capture_daemon.c`](camera/film_capture_daemon.c) 修正 `RAM_JOB_READ_RETRY` 後誤殘留 `uncertain=1` 導致守護行程提前停止的問題。
- **修正 Linux GCC / glibc 單元測試相容性**：
  - [`test_native_film_view.py`](test_native_film_view.py) 以 `model_syscall` 避開 ELF PLT 攔截並修正 `snprintf` 重疊緩衝未定義行為，確保在 macOS 與 Ubuntu Linux（GitHub Actions CI）皆能全數通過。
- **文件與排雷規範完整化**：
  - 擴充 [`docs/部署與操作規範.md`](docs/部署與操作規範.md) 第六節「完整實戰排雷手冊」，正式改推 **Windows 10/11 使用 WSL2 + `usbipd-win --auto-attach` 部署**（避開 Win11 原生 `libusb-win32`/Zadig 驅動問題），並補齊 `--pmca-dir`、`--install-hook`、`/disable_hook`/`/A6400/DISABLE`、24 kHz Sony SDS 格式要求、`/dev/nflasha10` 空間檢查與冷開機驗收步驟。

---

## [V3.5.1 Fast SP] - 2026-11-19

> 實機驗收基準：`package=5b079b295c2a7a27`；對應公開發布：`v3.5.1-fast-sp`

- **「膠片細節」子分頁（`FILM_MENU_DETAILS`）**：主選單新增第 3 列，可於拍攝期動態切換 **顆粒強度 `0–3`**（`無 (加速)`／`輕微`／`標準`／`濃厚`）與 **底片光暈 `開 / 關`**（關光暈時略過 `film_mask_blur`，`mask_kib=0`）及一鍵恢復預設。
- **編譯與演算法提速**：[`vendor/jpge.cpp`](vendor/jpge.cpp) 啟用 `-O3`、精簡 `DCT_MUL`、移除子閾值 64-bit 乘法、`inline put_bits`／`noinline flush_output_buffer`；[`camera/author_film.c`](camera/author_film.c) 預縮放 `bell_scaled[256]` 並將水平模糊改為 `O(1)` 滑動視窗；[`camera/film_ram_job.sh`](camera/film_ram_job.sh) 以 `busybox ls -ln` 單次 `lstat` 取代 `wc -c`。
- **公開建置與架構澄清**：將 `1.7MP / 6MP` 畫質切換自私人音效宏解耦、確認實機 `/proc/cpuinfo`（ARMv7 Cortex-A5 **無 NEON**）、補足 exFAT／FAT32 原生相容性、`1,024` 張佇列上限與連拍寫尾 `stage=source_size` 安全保護實證。
- **實機實拍耗時**：`1.7MP 極速` **`5,650 ~ 5,950 ms`**（`grain=0, halation=0`）；`6MP 高清` **`17,490 ms`**。

---

## [V3.5 Fast] - 2026-11-18

> 實機基準：`package=ff85fe8a9a317b68`

- 引入 `picojpeg` 8-bit Huffman 查表與 `6MP`（`3000×2000`）真半尺度 IDCT（跳過完整 8×8 逆變換）。
- 實機 `6MP` 由 `33.41 s` 大幅縮短至 `20,960 ms`，`1.7MP` 約 `8,160 ms`（預設 `grain=2, halation=1`）。

---

## [V3.4 Base LTR] - 2026-11-18

> 實機基準：`package=2bdd6b742789b8e7`

- 主選單左下角新增 **`畫質: 1.7MP 極速 / 6MP 高清`** 一鍵切換。
- 輸出檔名改為與原圖後 4 碼直觀對應之 `F<原圖4碼><3碼亂數>.JPG`。
- 新增完整機上渲染分段計時輸出 `RAM_JOB_TIMING`。

---

## [V3.2 Universal / Charo] - 2026-11-17

- 導入執行期動態抽取機身 `/usr/lib/libAK8975.so` 產出獨立掛勾（不帶走機身專有二進位）、動態掃描 `im.elf` PID（[`camera/im_pid_find.h`](camera/im_pid_find.h)），並支援選配私人快門音效掛載測試。

---

## [V3.1 CLite] - 2026-11-17 ~ 2026-11-18

> 歷史回滾基準：`package=77e49f2132ca0878`；對應公開發布：`v3.1-clite-toast-stable-20261118`

- 建立半透明深色浮層卡片與 Sony Alpha 橘條 UI 視覺定版、左下角 Toast 進度 HUD（`a6400_ui_toast_v1`）、`-O2` 編譯與精簡寫卡雜湊驗證，確立雙向 `<= 196,608 B` 純 RAM 開機包規範。

---

## [V0 / V1 / V2 Lite] - 2026-11-14 ~ 2026-11-16

- **V2 Lite**：確定純 RAM（`/tmp_bt`）掛載架構、原生 `mwf_monio` `IFSYS` 只讀原片＋非破壞寫入 `/A6400/TEST/`、以及獨立寫卡安全驗證與鎖定機制。
- **V0 / V1 Beta**：完成 `MENU + 放大鏡` 組合鍵召喚原型、`Appframework` UI 執行緒接駁與觸控回饋測試。
