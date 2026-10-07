# Tutikumo（土雲）

『モンスターハンターポータブル 2nd G』（ULJM-05500）の PSP 実行ファイルを、事前に C++ へ変換（静的リコンパイル）してネイティブに動かす移植です。エミュレーターではなく、ゲームのコードそのものを PC 向けにコンパイルし、その周りの PSP のシステム（カーネル、各種ライブラリ、GPU、音声、入力、セーブ、アドホック通信）を Tutikumo が受け持ちます。

ゲームのデータは含みません。手持ちの UMD から作った ISO が必要です。

## できること

| 機能 | 内容 |
| --- | --- |
| ネイティブ実行 | 本体（約17.5万命令）とオーバーレイ298個をすべて C++ に変換。変換されていないコードはインタプリタが受け持つ |
| HD 描画 | Vulkan で PSP の ×1〜×6、またはウィンドウの大きさに合わせて描画 |
| フレームレート向上 | ゲームは 30fps のまま、間のフレームを補間して 45/60/90/120fps やディスプレイのリフレッシュレートで表示 |
| 右スティックカメラ | 左右の回転と上下の傾きをアナログで操作（2G は本来十字キーでの左右のみ）。L で背後に戻すと傾きも戻る。マウスでも操作できる |
| HD テクスチャパック | PPSSPP 形式のパック（PNG、KTX2/Basis Universal、xxh64・xxh32・quick ハッシュ）を読み込む |
| 操作 | キーボード・マウス・ゲームパッドの割り当てを自由に変更できる。画面キーボードも用意 |
| セーブ | PSP と同じ形式で保存。取り込み・書き出し・バックアップができる |
| ムービー・音声 | オープニングムービー（H.264 / ATRAC3plus）と BGM・効果音 |
| 表示言語 | メニューとセットアップ画面は日本語と英語に対応（「システム > 言語」、初期値は Windows の言語）。ゲーム内の文章は変わりません |

## 使い方（Windows）

1. ビルドする（[docs/BUILDING.md](docs/BUILDING.md)）。
2. `Tutikumo.exe --install "ISO のパス"` で ISO を登録する。画面から選ぶ場合は、引数なしで起動するとセットアップ画面が開きます。
3. `Tutikumo.exe` を起動する。Esc（ゲームパッドなら L3+R3）でメニューが開きます。

データ（設定、セーブ、テクスチャパック、ログ）は `%APPDATA%\Tutikumo\MHP2G\` に置かれます。テクスチャパックは `textures\ULJM05500\` に入れます。

## Windows 11 の「スマートアプリコントロール」について

Tutikumo の実行ファイル（`Tutikumo.exe` と `overlays` の DLL 298個）にはコード署名がありません。Windows 11 のスマートアプリコントロールがオンになっていると、署名のない DLL を読み込むたびに確認が入り、一部がブロックされます。次のような症状が出ます。

- 「Windows セキュリティ」の通知が何度も出る
- 起動やエリアの読み込みがとても遅い、または止まる
- ログに `[overlay] cannot load ... error 4551` と出る

ウイルスとして検出されているわけではありません（Microsoft Defender のスキャンでは検出なし）。この場合は「Windows セキュリティ → アプリとブラウザーの制御 → スマートアプリコントロールの設定」でオフにしてください。オフにするかどうかはご自身で判断してください。

## フォルダ構成

```
include/psprecomp/   変換・実行の土台（汎用部分）
src/                 同上
tools/               解析ツールと C++ 生成ツール（psp_analyze, psp_recomp）
profiles/mhp2g/      2nd G 用の部分（PSP システムの実装、描画、UI、カメラなど）
docs/                仕組みとビルドの説明
```

変換された C++（`profiles/mhp2g/generated/`、`profiles/mhp2g/overlays/`）はゲームから作られるものなので、リポジトリには含めません。

## クレジット

Tutikumo は、TeamGDB による [Yakumo](https://github.com/TeamGDB/Yakumo)（『モンスターハンターポータブル 3rd HD Ver.』の静的リコンパイル移植）と、その土台の PSPRecomp をもとに作られています。MIT ライセンスに従い、元の著作権表示を [LICENSE](LICENSE) に残しています。同梱している第三者のソフトウェアは [profiles/mhp2g/packaging/THIRD_PARTY_NOTICES.md](profiles/mhp2g/packaging/THIRD_PARTY_NOTICES.md) と [docs/SOURCE_PROVENANCE.md](docs/SOURCE_PROVENANCE.md) にまとめています。
