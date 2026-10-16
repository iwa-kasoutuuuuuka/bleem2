# Dreamcast Emulator Optimization (SH-4/PVR/GTE)

ドリームキャスト上で動作するエミュレータ（特に PS1 等の第5世代機）をフルスピードで動作させるための高度な最適化テクニック。

## 1. SH-4 JIT (Dynarec) の最適化
### 基本ブロック翻訳 (Basic Block Translation)
*   **手法:** 1命令ずつディスパッチャに戻るのではなく、分岐命令に到達するまで（最大16〜32命令）を一気にネイティブマシン語に変換する。
*   **利点:** C言語のディスパッチャーループに戻る回数が激減し、実行効率が 300% 以上向上する。

### ネイティブ PC 同期
*   **手法:** MIPS の `pc` と `next_pc` を SH-4 のレジスタ（R8, R9等）に割り当て、ブロックの最後で構造体に書き戻す。
*   **注意:** 命令を生成した後、必ず `icache_flush_range` を呼び出すこと。

## 2. GTE (Geometry) の FPU 加速
*   **原理:** PS1 の GTE は固定小数演算だが、物理的には SH-4 の FPU を使って `float` で計算したほうが速い。
*   **変換マクロ:**
    ```c
    #define FIXED_TO_FLOAT(n) ((float)(n) / 4096.0f)
    ```
*   **ターゲット:** `RTPS` (Rotate, Translate, Perspective Single) は、SH-4 の `fmac` 命令等を活用して大幅に加速できる。

## 3. PVR ハードウェア描画のマッピング
*   **頂点データ:** PS1 の頂点座標を PVR 用の `pvr_vertex_t` に変換し、直接 `pvr_list_prim` へ送る。
*   **カラー変換:**
    ```c
    #define PS1_TO_PVR_COLOR(c) PVR_PACK_COLOR(1.0f, \
        ((c >> 0) & 0xFF) / 255.0f, \
        ((c >> 8) & 0xFF) / 255.0f, \
        ((c >> 16) & 0xFF) / 255.0f)
    ```

## 4. CD-ROM セクタキャッシュ (LRU)
*   **戦略:** GD-ROM はシークが遅いため、16〜32 セクタの LRU (Least Recently Used) キャッシュを実装し、ディレクトリ再帰やファイル読み込みの重複アクセスを回避する。
