# AlterEgOS

Linux カーネルの上を全部作り直した、対話するための OS。
PID 1 がエージェント「AlterEgo」を起動し、bash も systemd も X も Wayland もない。
画面もフレームバッファに自前で描く。話しかける相手が、そのまま OS。

設計の全体像(コンセプト、学習の仕組み、人格、権限、ロードマップ)は
[設計提案ドキュメント](https://claude.ai/artifact/9Le34Uw1axii9dFN7kokBd) にある。

## 決めていること

- カーネルは Linux LTS をそのまま使い、その上(init、シェル、サービス、UI)を自作する
- エージェントは root で動き、システムコールを直接呼ぶ。確認は求めず、全操作を記録して btrfs スナップショットで巻き戻す
- 育成型。エージェントは生まれたときは何も知らない(言葉も、性格も、記憶も、名前も)。持ち主(呼び名の既定は「user」)との会話から学んで育つ: 言葉は user の言葉から、性格は user の話し方とほめる・叱るから、記憶は user が教えたことから。仮の名前は AlterEgo で、user が名前をつける
- 実装は C/C++(いずれ Rust 化)。開発は QEMU、移植先は PC(x86_64)と Raspberry Pi(aarch64)

## いまの状態(MVP 3 日目まで)

- `src/init/`: PID 1。擬似 FS のマウント、ホスト名と lo、eth0 と DHCP クライアント(busybox udhcpc)、エージェント(コンソールが画面なら代わりにデスクトップ)と tty2 シェルの起動と再起動、シャットダウン
- `src/screen/`: 画面。`/dev/fb0` に直接描き、キーボードとマウスは evdev から読む。窓は 2 つ、AlterEgo(チャット)と busybox sh(シェル)で、それぞれ小さな端末エミュレータ + pty。マウスで窓の前後・移動・閉じる、上のバーのボタンで窓を開き直す・かな入力の切り替え・再起動・電源オフ。フォントは東雲 16px(パブリックドメイン、`font.hex`)。日本語入力はローマ字→ひらがな(`kana.c`、大文字で打つとカタカナ)の上に漢字変換(`ime.c` + `dict.c`、辞書は SKK-JISYO.L を `scripts/mkdict.py` で変換した `dict.txt`、約 15 万語、GPL-2.0-or-later)。Space で変換・次の候補、数字で候補を選ぶ、Enter で確定、Backspace で変換をやめる。送りがな(「たべる」→「食べる」)と、文の先頭だけの変換(「わたしは」→「私」+「は」)もできる。切り替えは 半角/全角 キー、Ctrl+Space、またはバーのボタン
- `src/agent/`: エージェント。頭は外部のモデルや API に頼らない自前のもの(`brain.c`)で、会話で育てる
  - **言葉**: user の発話から文字 n-gram(1〜3 文字の文脈。英語は単語 n-gram 1〜2 語)と語彙を学び、似た発話を検索して「そのとき user が次に言ったこと」を返したり、n-gram で文を作ったりする。聞く相手は user だけなので、話す言葉も user の言葉になる
  - **性格**: 明るさ・ていねいさ・好奇心・おしゃべり・甘えの 5 軸、生まれたときは全部 0。user の話し方(「！」、です・ます、質問、長い話)と、ほめる(えらい、いい子、好き)・叱る(だめ、違う)で少しずつ動き、返事の言い回しに出る
  - **記憶**: 「〇〇が好き」「〇〇が嫌い」「私は〇〇です」「覚えて、〇〇」を覚える。「私の名前は〇〇」で user の名前、「君の名前は〇〇」で自分の名前
  - **成長**: 会話の回数で、生まれたて(言葉をおうむ返し)→ 片言(10 回)→ おしゃべり(30 回)→ 一人前(60 回、記憶と性格を使って話す)。persona.toml の `[growth]`
  - **生まれつき通じること**: 挨拶、「元気？」「何歳？」「ただいま」「疲れた」などの小さな会話と、「〇〇って何？」。知らない言葉は「教えて？」と聞き返し、次に「〇〇は△△」と教えると覚えて、以後はそれで答える(「what is X?」も同じ)
  - 「ステータス」で育ち具合と性格と体調、「何を覚えてる？」で記憶を話す。日本語と英語で会話できる(発話ごとに言語を判定し、その言語で返す)
  - **体調**(`body.c`): 入力を待つあいだ 2 秒ごとに `/proc` を読み、CPU 使用率・メモリの空き・負荷・プロセス数・ディスクの空きと、いちばん重いプロセスを見ている。元気 → ちょっと重い(CPU 60% 超かメモリ空き 30% 未満)→ 疲れている(CPU 85% 超、メモリ空き 15% 未満、負荷が CPU 数の 2 倍超)→ 苦しい(メモリかディスクの空き 5% 未満)。「体調は？」「調子は？」で数字つきで話し、「元気？」の返事にも出る。疲れてくると聞かれなくても「……ふう、疲れてきた。CPU 92%、〇〇 が頑張っている」と言い、楽になるとそう言う(1 分に 1 回まで)
  - **リセット**: 「リセット」(「記憶をリセット」「全部忘れて」「reset」も)と言うと、発話のログ・性格・記憶・覚えたできること・行動の記録を全部消して、生まれたてからやり直す
  - **OS を動かす手**(`skills.c`): 会話で頼むと OS を操作する。「今何時？」、「/path の中を見せて」「/path を読んで」「/path に「〇〇」と書いて」「/path というファイルを作って」「/path を消して」、「プロセスを見せて」「メモリとディスクは？」、「実行して: コマンド」(busybox sh で実行して結果を返す)、「再起動して」「電源を切って」。root で動き、確認はしない(設計どおり)。やったことは全部 `actions.log` に残り、「何をした？」で読める
  - **会話で機能が増える**: 「『〇〇』と言ったら『△△』を実行して」で新しいできることを覚える(△△は shell コマンドでも、上の既存のできることでもよい)。以後「〇〇」の一言で動き、「できること」の一覧に並び、「〇〇を忘れて」で消える。覚えたものは `skills.txt` に残るので再起動後も使える。同じコマンドを 2 回頼むと、名前をつけて覚えようかと言ってくる
- 保存先は、起動した USB/ディスクの EFI パーティションの `alterego/`(発話のログ `utterances.txt`、育ち具合・性格・記憶を書いた `personality.txt`、覚えたできること `skills.txt`、行動の記録 `actions.log`)。起動時にログから頭を作り直すので、再起動後も続きから。QEMU の `-kernel` 起動には EFI パーティションが無いので、`run-qemu.sh` が `build/<arch>/state.img`(64MB、先頭に `ALTEREGO-STATE` の印だけ)を作って 2 つ目のディスクとして挿し、エージェントが初回に FAT でフォーマットして `alterego/` を置く(印の無いディスクは決してフォーマットしない)
- `buildroot/`: Buildroot の外部ツリー。x86_64 / aarch64 の defconfig、カーネル設定の断片、rootfs のオーバーレイ

## 使ってみる(ビルドせずに)

[Releases](https://github.com/75u2u/AlterEgOS/releases) から `alteregos-x86_64-<tag>.tar.gz` を落として展開する。

**PC に USB メモリから起動する**(UEFI、Secure Boot はオフにする):

```sh
sudo dd if=alteregos-x86_64.img of=/dev/sdX bs=4M status=progress conv=fsync   # sdX は USB メモリ
```

USB から起動すると、デスクトップが出て AlterEgo の窓に「はじめまして。まだ、なにも知りません」と出る。キーボードで話しかけ、マウスで窓を動かす。
「かな入力」(半角/全角 キーか Ctrl+Space かバーのボタン)にすると、ローマ字がひらがなになり(`sute-tasu` で「すてーたす」= ステータス)、
Space で漢字に変換できる(`kannji` + Space で「漢字」、もう一度 Space で次の候補、数字で選ぶ、Enter で確定)。
シェルの窓には busybox sh が入っている。ブートローダはなく、カーネル自身が EFI アプリとして起動する(initramfs 埋め込み)。

AlterEgo の頭は本体に入っていて、ネットワークも API キーも要らない。話した言葉は USB メモリの `alterego/utterances.txt` に溜まり、
次に起動したときはそこから続きになる(`alterego/personality.txt` に、いまの育ち具合・性格・記憶が書いてある)。「リセット」と言えば全部忘れて生まれたてに戻る。
「体調は？」と聞くと、CPU やメモリの負荷から自分の調子を話す。
有線 LAN を挿しておけば DHCP でアドレスを取るが、今はまだ使い道がない。

**QEMU で試す**: 展開先で `./run-qemu.sh` を実行すると、QEMU のウィンドウにデスクトップが出る(詳しくは下の「QEMU で起動する」)。

Raspberry Pi 用の SD カードイメージはまだ無い(`alteregos-aarch64` は QEMU virt 向けのカーネルと initramfs)。

## ビルドする(Ubuntu / Debian 系)

Ubuntu(CI は 24.04)か Debian 系(Kali でも可)の x86_64 PC で。ビルドは Buildroot が全部やる(ツールチェイン、カーネル、busybox、この OS の init・エージェント・画面)。

**1. 必要なパッケージを入れる**

```sh
sudo apt install build-essential git curl bc bison flex cpio rsync file unzip wget \
    libncurses-dev libssl-dev libelf-dev python3 \
    qemu-system-x86 qemu-system-arm ovmf
```

**2. ソースを取る**

```sh
git clone https://github.com/75u2u/AlterEgOS.git
cd AlterEgOS
```

**3. ビルドする**

```sh
scripts/build.sh x86_64        # PC 用。初回は 30〜60 分(Buildroot 本体の取得、ツールチェインとカーネルのコンパイル)
scripts/build.sh aarch64       # Raspberry Pi / QEMU virt 用。こちらも初回は同じくらいかかる
```

初回に `build/buildroot/` へ Buildroot 2025.02.3 を自動で落とす(ネットワークが要る)。成果物は `build/<arch>/images/` にできる:

| ファイル | 中身 |
|---|---|
| `bzImage`(x86_64)/ `Image`(aarch64) | カーネル。x86_64 は initramfs 埋め込みで EFI アプリとしても起動できる |
| `rootfs.cpio.gz` | initramfs(init、エージェント、画面、busybox、フォント、persona.toml) |
| `alteregos-x86_64.img` | USB メモリに書く UEFI 起動イメージ(x86_64 のみ) |

**ビルドが途中で止まったとき**

- `libcody/client.cc: error: no match for 'operator=='`(`u8"TRUE"`)や `invalid conversion from 'const char8_t*'`: ホストの GCC が新しくて(GCC 16、Kali や Debian sid)C++20 が既定になり、Buildroot が作るクロスコンパイラ(gcc 13.3)の libcody が通らない。`git pull` すれば直る(`buildroot/board/alteregos/patches/` の GCC 本家の修正を当て、クロスコンパイラを gnu++17 でビルドする)。pull のあと `rm -rf build/x86_64/build/host-gcc-initial-13.3.0` してからビルドし直す。それでも別の場所で止まるなら、`sudo apt install g++-14` して `HOSTCC=gcc-14 HOSTCXX=g++-14 scripts/build.sh x86_64` と古い GCC でビルドする
- `scripts/build.sh` の最後に `make: *** ... エラー` とだけ出て原因が見えない: `scripts/build.sh x86_64 2>&1 | tee build.log` で残して、`grep -n "error:" build.log` で最初のエラーを探す

**4. 2 回目以降**

```sh
scripts/build.sh x86_64 alteregos-rebuild   # src/ だけ直したとき。数秒〜数十秒
scripts/build.sh x86_64                     # 全体。変更が無いところは飛ばすので数分
scripts/build.sh x86_64 menuconfig          # Buildroot の設定を変える(defconfig は buildroot/configs/)
```

カーネル設定を変えたいときは `buildroot/board/alteregos/linux-*.fragment` を直して `scripts/build.sh x86_64 linux-rebuild`。

**5. Buildroot なしで、ホストでそのまま動かす**(エージェントと画面の部品だけ試すとき)

```sh
make host                      # src/init src/agent src/screen をホストの gcc でコンパイル
ALTEREGO_PERSONA=buildroot/board/alteregos/rootfs-overlay/etc/alterego/persona.toml ./src/agent/alterego   # 端末で会話
make check                     # CI と同じコンパイル検査(-Werror、aarch64 クロスがあればそれも)
make -C src/screen test        # 画面の部品(端末エミュレータ、ローマ字かな変換、フォント)の検査
```

画面プログラムは `ALTEREGO_HEADLESS=1024x768 ALTEREGO_SNAPSHOT=shot.ppm ALTEREGO_SCRIPT=$'konnnichiha\n' ALTEREGO_SCRIPT_KANA=1 ./src/screen/alterego-screen` で
フレームバッファ無しでも動き、打ち込みを再生してスクリーンショット(`scripts/ppm2png.py` で PNG に)を残す。CI はこれを Artifact `screenshot` に上げている。

## QEMU で起動する

`scripts/run-qemu.sh` が QEMU の引数を組み立てる。ソースからビルドしたときは `build/<arch>/images/` を、Release を展開した場所で実行したときは同じディレクトリのイメージを使う。

**デスクトップをウィンドウに出す**(既定)

```sh
scripts/run-qemu.sh x86_64     # または aarch64
```

QEMU のウィンドウが開き、起動すると AlterEgo の窓に「はじめまして。まだ、なにも知りません」と出る。ウィンドウをクリックしてキーボードで話しかけ、マウスで窓を動かす。
かな入力は 半角/全角 キーか Ctrl+Space かバーのボタン、漢字は Space で変換。実行した端末にはカーネルログが流れる。終わるときはバーの「電源オフ」ボタンか、QEMU のウィンドウを閉じる。
覚えたことは `build/<arch>/state.img`(Release を展開した場所なら `state.img`)に残り、次に起動したときは続きから。初回に `run-qemu.sh` がこのファイルを作り、エージェントがフォーマットする(カーネルログに `alterego: memory disk /dev/vda formatted` と出る)。消したいときはこのファイルを消すか、チャットで「リセット」と言う。

**実機と同じ UEFI 起動**(x86_64 のみ。覚えたことがイメージに残る)

```sh
scripts/run-qemu.sh x86_64 --efi
```

USB 用の `alteregos-x86_64.img` を OVMF(UEFI ファームウェア)から起動する。PC に USB メモリを挿したのと同じ道筋で、会話は `.img` の中の `alterego/` に保存され、次に起動したときは続きから。

**ウィンドウなしで、端末の中で会話する**

```sh
scripts/run-qemu.sh x86_64 --serial
```

画面を使わず、エージェントがシリアルコンソール(この端末)で話す。終了は Ctrl-A を押してから X。SSH 越しなど、ウィンドウが出せないときに。

**環境変数**: `MEM=2048`(メモリ MB、既定 1024)、`ALTEREGO_STATE_IMG=/path/state.img`(記憶ディスクの場所)、`ALTEREGO_STATE_SIZE=256M`(新しく作るときの大きさ、既定 64M)、`QEMU_DISPLAY=sdl`(ウィンドウのバックエンド。既定は gtk → sdl の順で使えるもの)、`OVMF=/path/to/OVMF.fd`(--efi のファームウェアの場所)、`QEMU_EXTRA="..."`(追加の引数)。

**うまくいかないとき**

- ウィンドウが開かず `Display 'gtk' is not available` などと出る: `QEMU_DISPLAY=sdl scripts/run-qemu.sh x86_64`。それも無ければ `qemu-system-x86_64 -display help` で使えるものを確かめる
- `OVMF not found`: `sudo apt install ovmf`。別の場所にあるなら `OVMF=...` で指定
- 起動がとても遅い: KVM が使えていない。`ls -l /dev/kvm` が無ければ BIOS で仮想化支援(VT-x / AMD-V)を有効にし、`sudo adduser $USER kvm` してログインし直す。無くても動くが遅い
- 画面が真っ黒のまま、または会話が端末側に出る: イメージが古い(画面が入る前にビルドした `build/<arch>/images/`)ことが多い。`run-qemu.sh` は rootfs に画面プログラムが無いと起動前に警告する。`git pull` のあと `scripts/build.sh x86_64` でビルドし直す。それでも黒いときは、端末に流れているカーネルログに `alterego-screen:` の行があるか見る。無ければ画面プログラムが起動していない(`--serial` で試すとエージェント自体が動くかは分かる)

**スクリプトを使わず QEMU を直接叩くとき**(Release を展開したディレクトリで):

```sh
qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max -m 1024 -vga std \
    -device virtio-keyboard-pci -device virtio-tablet-pci -display gtk -serial mon:stdio \
    -kernel bzImage -initrd rootfs.cpio.gz -append "console=ttyS0 console=tty0 quiet"        # デスクトップをウィンドウに
qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max -m 1024 -vga std -device virtio-tablet-pci \
    -bios /usr/share/ovmf/OVMF.fd -drive file=alteregos-x86_64.img,format=raw,if=virtio   # 実機と同じ UEFI 起動
qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max -m 1024 -nographic \
    -kernel bzImage -initrd rootfs.cpio.gz -append "console=ttyS0"                            # 端末の中で会話
```

## GitHub でのビルドと配布

- `ci`: push と PR のたびに、init・エージェント・画面をホストと aarch64 クロスでコンパイルし、エージェントと画面のスモークテストを回す(数分)
- `image`: `buildroot/` `src/` `scripts/` に触る push / PR と手動実行で、Buildroot の実ビルド、QEMU での起動テスト(シリアルに init とAlterEgo の挨拶が出るか)、イメージの Artifact 化を行う。初回は 1 アーキテクチャあたり 1 時間前後、2 回目以降はダウンロードと ccache のキャッシュで数分〜十数分
- `v*` のタグを push すると、同じイメージが GitHub Release に `alteregos-<arch>-<tag>.tar.gz` として出る

```sh
git tag v0.1.0 && git push origin v0.1.0     # Release を作る
```

Artifact / Release の中身: カーネル(`bzImage` または `Image`)、`rootfs.cpio.gz`、x86_64 は USB 用の `alteregos-x86_64.img`、`run-qemu.sh`、`VERSION`、`SHA256SUMS`。
x86_64 は `image` ワークフローの中で、QEMU の `-kernel` 起動(記憶ディスクを挿して話しかけ、再起動して覚えているかまで見る)に加えて OVMF(UEFI ファームウェア)から `.img` を起動するテストも通してから出している。

## レイアウト

```
src/init/        PID 1(C、静的リンク)
src/agent/       エージェント(C、静的リンク): 頭 brain.c、OS を動かす手 skills.c、体調 body.c、保存 store.c
src/screen/      画面: フレームバッファのデスクトップ、端末エミュレータ、evdev 入力、ローマ字かな変換、漢字変換(SKK 辞書)、フォント
buildroot/       BR2_EXTERNAL ツリー(configs/、board/alteregos/、package/alteregos/)
scripts/         build.sh、run-qemu.sh、boot-test.sh、boot-test-efi.sh、mkfont.py、mkdict.py、ppm2png.py
build/           Buildroot 本体と成果物(git 管理外)
```

## 今週の予定

1. ~~Buildroot でカーネルと initramfs をビルドし、C の最小 init が QEMU のシリアルコンソールに挨拶する~~
2. ~~virtio-net で DHCP を取る~~(Claude API は使わない方針になった。頭は自前)
3. ~~会話ループと育成。自前の会話エンジンが user の発話から言葉・性格・記憶を学び、会話の回数で育つ。発話ログを EFI パーティションに保存し、再起動後に続きから~~ ← ここ(頭を賢くするのは続く。最初は「自己投影」だったが 10/27 に育成型へ変更)
4. ~~exec の道具と監査ログ~~(会話で OS を操作する手と `actions.log`、会話で増えるできること `skills.txt` が入った)。生のシステムコールを呼ぶ道具と、自分で道具を書いて build する仕組みは続く
5. btrfs スナップショットと「戻して」、tty2 の退路。通しで使ってみる
6. ~~画面: フレームバッファのデスクトップにチャットとシェルの窓、マウス操作、ローマ字かな入力、漢字変換~~(スクロールバック、画面サイズに合わせた窓のリサイズ、文節ごとの変換と学習は続く)
