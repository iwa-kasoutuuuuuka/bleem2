# bleem2 — PlayStation 1 emulator for Sega Dreamcast (KallistiOS)

ドリームキャスト上で PS1 ソフトを動かすことを目指すエミュレータです。2026/10/15 にコアを再実装し、Metal Slug X のミッション 1 が操作可能 (移動・ジャンプ・射撃・敵との交戦) な状態まで到達しました。
OpenBIOS による実 BIOS ブートから Metal Slug X の起動・描画まで到達しました (`DEBUG_LOG.md` 参照)。

## 現在の状態
- CPU: R3000 インタプリタ + 基本ブロック JIT (`src/jit2.c`, `USE_JIT=1`、検証モード `JIT_VERIFY` あり)。ブロック連結、動的ジャンプのインラインキャッシュ、ポーリングループ検出による早送り、IsC モード用ブロック。
- GPU: ソフトウェアラスタライザ (テクスチャ/CLUT 対応)。表示は PVR テクスチャ経由 (`src/display.c`)。
- CD-ROM: BIN/CUE (2352/2048) 読み出し、コントローラエミュレーション、DMA。
- SPU: 24 ボイス ADPCM (ピッチ/ADSR/ループ)。既定 (`SPU_AICA 1`) では各ボイスを AICA のハードウェアチャンネルで再生 (SH4 は ADPCM デコードのみ、ADSR は AICA の AEG にマッピング、リバーブはソフト実装を ch24/25 で再生)、`SPU_AICA 0` でソフト混合 (snd_stream)。リバーブ/XA は未対応。
- 速度 (Flycast Sh4Clock=200 = 実機相当, 2026/10/16): ゲーム中 CPU 約 14〜17ms/フレーム + 表示 2.2ms + SPU 約 2ms → リリース構成で 1 フレーム約 18〜21ms (45〜55fps 相当)。ポーリングループ検出 (`JIT_POLL_SKIP`) により CPU の実処理は約 4〜8 万サイクル/フレーム。実機未検証。
- メモリーカード: `src/memcard.c` (VMU に RLE 保存)。OpenBIOS のカードドライバが毎フレーム 140 バイトのカードアクセスを行い IRQ 負荷が大きいため既定では無効 (`MEMCARD_ENABLED`)。
- ビルド切替 (`src/mips.h`): `RELEASE_BUILD` (HUD/サンプラ/シリアル停止)、`AUTOPLAY` (フレーム番号でのパッド入力スクリプト)、`JIT_VERIFY`。

## ビルド
MSYS2 bash から (一時ディレクトリの指定が必要):
```bash
export TMP=/tmp TEMP=/tmp LANG=C
cd /d/DC/bleem2 && source ../KallistiOS/environ.sh && make && make cdi
```
`build/bleem2.cdi` に OpenBIOS (`psbios/openbios.bin`) とゲームイメージ (`build/Metal Slug X (USA).bin`) が同梱されます。

## 動作確認 (Flycast)
```powershell
powershell -File scripts/run_and_capture.ps1 -Seconds 300
```
`AUTOPLAY=1` (既定の dev 構成) ではフレーム番号ベースのパッド入力スクリプト (main.c) でタイトル→ミッション 1 まで自動進行し、300 フレーム毎の `WIN` 行 (emu/表示/総フレーム時間の平均) が VMU の `BLEEMWIN` に残ります (`scripts/read_vmu_log.ps1 -Name BLEEMWIN`)。性能比較は同じフレーム区間の `WIN` 行同士で行ってください。Flycast は `Sh4Clock = 200` (emu.cfg) で実機相当になります。
Flycast を起動し、ゲーム画面とシリアルウィンドウを 2 秒毎に `build/shots/` へ保存、終了後に VMU 内のログ
(`BLEEMLOG`) を表示します。画面上部の HUD に PC / FPS / 例外数、下部に直近のログが出ます。

## 主要ファイル
| ファイル | 内容 |
|---|---|
| `src/cpu.c` | MIPS インタプリタ |
| `src/mem.c` | メモリマップ・I/O ルーティング・TTY 捕捉 |
| `src/cdrom.c` | ディスクイメージ / ISO9660 / CD-ROM コントローラ |
| `src/gpu.c` | ソフトウェア GPU |
| `src/dma.c` `src/timers.c` `src/sio.c` | DMA / ルートカウンタ / パッド (2 ポート) |
| `src/memcard.c` | メモリーカード (VMU 保存, 既定で無効) |
| `src/display.c` | PS1 VRAM → PVR テクスチャ表示 (ストアキュー) |
| `src/log.c` | HUD・VMU ログ |
| `src/jit2.c` | MIPS→SH-4 ブロック JIT |
| `src/spu.c` | SPU (ADPCM ミキサ) |
| `src/hle.c` | HLE BIOS (フォールバック, `BOOT_AUTOBOOT=1`) |
| `scripts/` | 自動実行・キャプチャ・ログ抽出スクリプト |
