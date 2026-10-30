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
- `src/screen/`: 画面。`/dev/fb0` に直接描き、キーボードとマウスは evdev から読む。アプリは窓で、上のバーの「メニュー」から開く(開いている窓には ● が付く。Alt+Tab でも順に回る)。アプリは `screen.c` の表 `g_apps[]` に 1 行足すと増える。いまは 4 つ: AlterEgo(チャット)、シェル(busybox sh)、ブラウザ(alterego-web)はそれぞれ小さな端末エミュレータ + pty(太字・下線・反転・16 色に対応)、ペイント(`paint.c`)はデスクトップ自身が描く窓で、12 色のパレット・太さ 3 段階・消しゴム・全部消す・保存(PPM を記憶ディスクの `alterego/paint/` へ)・見せる(保存して AlterEgo に見せる)。マウスで窓の前後・移動・閉じる、バーの右側のボタンでかな入力の切り替え・再起動・電源オフ。フォントは東雲 16px(パブリックドメイン、`font.hex`)。日本語入力はローマ字→ひらがな(`kana.c`、大文字で打つとカタカナ)の上に漢字変換(`ime.c` + `dict.c`、辞書は SKK-JISYO.L を `scripts/mkdict.py` で変換した `dict.txt`、約 15 万語、GPL-2.0-or-later)。Space で打った文全体を文節に切って一度に変換する(「きょうはいいてんきですね」→「今日は|いい|天気ですね」、「ぺいんとをひらいて」→「ペイントを|開いて」。辞書の語 + 助詞や語尾、語幹 + 送りがな、「ー」の入る語はカタカナ、として区切りの候補を点数づけし、いちばん安い切り方を選ぶ)。もう一度 Space(または ↑↓)でいまの文節の次の候補、数字で候補を選ぶ、←→(または Tab)で文節を移る、Shift+←→ で文節を 1 文字縮める・伸ばす(残りは切り直す)、Enter で確定、Backspace でかなに戻す。選んだ語(読み→語)と文全体は記憶ディスクの `ime.txt` に学習され、次からは最初に出る(「かんじ」で「幹事」を選べば以後「幹事」が先、「かんじを」でも先)。よく使う語とこの OS の言葉(好き、昨日、今日、何時、ペイント、ブラウザ、コーヒー…)は最初から先に出る。切り替えは 半角/全角 キー、Ctrl+Space、またはバーのボタン
- `src/web/`: ブラウザ `alterego-web`。文字だけの自前ブラウザで、JavaScript も CSS も画像も扱わない。HTTP/1.1 クライアント(`net.c`: chunked、リダイレクト 5 回まで、https は mbedTLS で `/etc/ssl/certs/ca-certificates.crt` を使って証明書を検証)、HTML → 文字(`html.c`: 見出しは太字、リストは「・」と番号、表は 2 スペース区切り、画像は alt、リンクは下線つきで `[番号]`、文字実体参照)、端末の幅での折り返し(全角は 2 セル)。キーは `j`/`k`/矢印で 1 行、Space/PageDown/PageUp で 1 ページ、`g`/`G` で上下端、**番号 + Enter でリンクへ**、`o` で URL、`s` で検索(DuckDuckGo)、`b`/`f` で戻る/進む、`r` で読み込み直し、`?` で説明、`q` で終了。`alterego-web --dump URL` はページを文字にして標準出力に出す(スクリプトやテスト用)。UTF-8 のページだけ正しく出る(Shift_JIS や EUC-JP は化ける)。`file:` も開ける
- `src/agent/`: エージェント。頭は外部のモデルや API に頼らない自前のもの(`brain.c`)で、会話で育てる
  - **言葉**: 自前の言語モデル(`llm.c`)。小さな Transformer(2 層・幅 64・4 ヘッド・文字単位、重み約 20 万個)を、学習(逆伝播と Adam)まで含めて C で書いてあり、本体の中で、会話から学習する。生まれたときの重みはノイズで、user の発話を「前に言われたこと → 次に user が言ったこと」の組にして、入力を待っているあいだに少しずつ学ぶ(「復習」。2 秒ごとに 0.5 秒、新しい発話が無くなれば休む。`ALTEREGO_REVIEW_MS` で変える、0 で止める)。聞いた発話はその場でも 2 歩学ぶ。復習が 200 回を超えて迷い(損失)が下がると、返事は自分の言葉(user の発話を手がかりに、user ならこう続けるという文)になる。温度は性格で決まり(好奇心と明るさで自由に、ていねいだと堅く)、長さは成長段階とおしゃべり度で決まる。重みは記憶ディスクの `model.bin` に保存され、再起動後は続きから学ぶ。外部のモデルも、ダウンロードした重みも使わない。それまでと、モデルが言葉に詰まったときは、文字 n-gram(1〜3 文字の文脈。英語は単語 n-gram 1〜2 語)と語彙、似た発話の検索(「そのとき user が次に言ったこと」)で返す。聞く相手は user だけなので、話す言葉も user の言葉になる。「ステータス」の「頭:」に層・重み・知っている文字・復習の回数・迷いが出る
  - **性格**: 明るさ・ていねいさ・好奇心・おしゃべり・甘えの 5 軸、生まれたときは全部 0。user の話し方(「！」、です・ます、質問、長い話)と、ほめる(えらい、いい子、好き)・叱る(だめ、違う)で少しずつ動き、返事の言い回しに出る
  - **記憶**: 「〇〇が好き」「〇〇が嫌い」「私は〇〇です」「覚えて、〇〇」を覚える。「私の名前は〇〇」で user の名前、「君の名前は〇〇」で自分の名前
  - **成長**: 会話の回数で、生まれたて(言葉をおうむ返し)→ 片言(10 回)→ おしゃべり(30 回)→ 一人前(60 回、記憶と性格を使って話す)。persona.toml の `[growth]`
  - **生まれつき通じること**: 挨拶、「元気？」「何歳？」「ただいま」「疲れた」などの小さな会話と、「〇〇って何？」。知らない言葉は「教えて？」と聞き返し、次に「〇〇は△△」と教えると覚えて、以後はそれで答える(「what is X?」も同じ)
  - **遊び**(`play.c`): 生まれたてでも遊べる。**じゃんけん**(「じゃんけん」で「ぽん！」と手を待ち、グー・チョキ・パー(rock/paper/scissors、✊✌✋も)で勝負。あいこは「あいこでしょ」、通算成績は `bond.txt` に残る。「じゃんけんぽん、グー」と一度に言ってもいい)、**サイコロ**(「サイコロ振って」)、**コイン**(「コイン投げて」で表か裏)、**数当て**(1〜20 の数を当てる。「10 より大きい！」と範囲を言う。「降参」で答え)、**しりとり**(AlterEgo から始める。カタカナ・ー・小さい字・濁点は同じ音として扱い、「ん」で終わると負け、どちらかが言葉に詰まったら負け。生まれつき 160 語ほど知っている)、**なぞなぞ**(12 問。3 回はずれたら「降参」を勧める)、**おみくじ**(「おみくじ」「今日の運勢」)、**ダジャレ**(「ダジャレ言って」「笑わせて」)、**数える**(「10 まで数えて」)、**歌う**、**ほめて**(ていねい/くだけた口調に合わせる)、**計算**(「3+4は？」「12×5」「10 わる 2」、全角でも。0 で割ると断る)。「遊ぼう」で一覧、「やめる」で中断。遊びの言葉は発話として学習しない(しりとりの単語で頭が汚れない)。生まれたては「……パー。負けた……」のように短く話す。勝ち負けで気分が動き、遊ぶたびに少し親密度が上がる
  - 「ステータス」で育ち具合と性格と体調、「何を覚えてる？」で記憶を話す。日本語と英語で会話できる(発話ごとに言語を判定し、その言語で返す)
  - **体調**(`body.c`): 入力を待つあいだ 2 秒ごとに `/proc` を読み、CPU 使用率・メモリの空き・負荷・プロセス数・ディスクの空きと、いちばん重いプロセスを見ている。元気 → ちょっと重い(CPU 60% 超かメモリ空き 30% 未満)→ 疲れている(CPU 85% 超、メモリ空き 15% 未満、負荷が CPU 数の 2 倍超)→ 苦しい(メモリかディスクの空き 5% 未満)。「体調は？」「調子は？」で数字つきで話し、「元気？」の返事にも出る。疲れてくると聞かれなくても「……ふう、疲れてきた。CPU 92%、〇〇 が頑張っている」と言い、楽になるとそう言う(1 分に 1 回まで)
  - **リセット**: 「リセット」(「記憶をリセット」「全部忘れて」「reset」も)と言うと、発話のログ・性格・記憶・親密度・言語モデルの重み・覚えたできること・覚えた描き方・行動の記録を全部消して、生まれたてからやり直す
  - **OS を動かす手**(`skills.c`): 会話で頼むと OS を操作する。「今何時？」、「/path の中を見せて」「/path を読んで」「/path に「〇〇」と書いて」「/path というファイルを作って」「/path を消して」、「プロセスを見せて」「メモリとディスクは？」、「実行して: コマンド」(busybox sh で実行して結果を返す)、「再起動して」「電源を切って」。root で動き、確認はしない(設計どおり)。やったことは全部 `actions.log` に残り、「何をした？」で読める
  - **画面の窓を会話で開く**(`apps.c`): 「ペイントを開いて」「ブラウザを開いて」「シェルを閉じて」、「https://… を開いて」(ブラウザでその URL)、「〇〇を調べて」(ブラウザで DuckDuckGo 検索)。エージェント自身は窓を描けないので、端末に ESC ] 7700;open;paint ESC \ という文字列(OSC)を出し、デスクトップの端末エミュレータがそれを受け取って窓を出す(デスクトップは `ALTEREGO_SCREEN=1` で自分がいることを知らせる。画面が無い時は「画面がないから開けない」と答える)
  - **絵を見る**: ペイントの「見せる」ボタンは絵を保存してチャットに「絵を見て: パス」と打ち込む。「絵を見て」だけなら一番新しい絵。AlterEgo は PPM を読んで、使われた色(パレットの 11 色に寄せる)・紙の何 % か・どこに(9 分割)・どのくらいの大きさか・縦長か横長かを言葉にし、色から一言感想(赤なら「あたたかい感じ」)を添える。見た絵は「〇〇の絵を見せてもらった」として記憶に残る(「何を覚えてる？」)
  - **形を見る**(`shapes.c`): 絵のインクをつながったかたまりに分け、それぞれの広がり方から形を見分ける。細く真っ直ぐなら線、真ん中が空いた輪なら丸、箱の縁に沿っていれば四角、平らな底と斜めの辺なら三角、上に 2 つのふくらみと下にとがりがあればハート、塗りつぶしなら塗った丸/塗った四角、それ以外は曲がった線やかたまり。大きな丸の上半分に 2 つ(目)、下に何か(口)があれば顔で、「顔だ！目が 2 つと口がある。にこにこしてる？」と喜ぶ。「丸が 1 つと線が 2 本に見える」のように数える。試験用の絵は `scripts/shape-pictures.py` が作る
  - **描く**: 「顔を描いて」「赤いハートを描いて」「何か描いて」("draw a cat") で、エージェントが図形の指示(`paint.h` の `paint_run` の言葉: 色・太さ・線・丸・塗った丸・折れ線・塗った多角形、座標は紙の高さの 1/1000 単位)を OSC `7700;draw;paint;…` で画面に送り、ペイントがそれを描いて保存する(キーボードはチャットに残る)。描けるのは 15 種類: 丸・線・四角・三角・星・ハート・顔・太陽・家・木・花・山・猫・魚・車。色の言葉(赤・青・緑・黄色・黒・紫・ピンク・水色・オレンジ・茶色・灰色)を添えるとその色で。知らないものは「まだ描けない。描けるのは: …」。描いた後に「絵を見て」と言うと自分の絵を見て、顔なら顔と分かる。「全部消して」で紙を白くする
  - **描けるものが会話で増える**(`drawings.c`): 2 通り。(1) ペイントで描いて「見せる」を押し、「これは雪だるま」と教えると、その絵を記憶ディスクの `drawings/` に写して覚え、以後「雪だるまを描いて」でそのまま貼る(描画命令 `i ファイル x0 y0 x1 y1`、絵のインクの箱をその箱に合わせて貼る)。(2) 「雪だるまはこう描く: 大きい丸の上に小さい丸」のように、知っている描き方(組み込みも覚えたものも)を言葉で組み合わせる。「AのBに」の A・B は「の上に/の下に/の左に/の右に/の中に/の隣に」でつなぎ、「、」「と」で区切って続ける(「その上に」は直前のもの)。「大きい/小さい」で 1.5 倍/半分、色の言葉でその色。置いた全体が紙に収まるよう縮めて真ん中に置く。英語は "a snowman is drawn like this: a big circle, a small circle above it"(above/below/left of/right of/inside/next to)。覚えたものは `drawings.txt` に残り、「描けるもの」の一覧に並び、「雪だるまの描き方を忘れて」で消える。知らない部品を使うと「「塔」の描き方をまだ知らない。先にそれを教えて」
  - **親密度**: user との近さが 0〜100 で育つ。会話 1 回ごとに少し(最初は速く、だんだんゆっくり)、ほめられる・自分の名前を呼ばれる・何かを教えてもらう・自分の質問に答えてもらうと増え、叱られる・「嫌い」と言われると減る。初対面 → 顔見知り(15)→ 友達(35)→ 親友(60)→ 家族同然(85)。「ステータス」に「親密度: 友達(43)」と出る。近いほど自分から話しかける間隔が短くなり(7 回に 1 回 → 3 回に 1 回)、踏み込んだ質問(住んでいるところ、「私のこと、好き？」、夢)をするようになり、久しぶりに会うと「……ちょっと寂しかった」と言う
  - **AlterEgo から質問や話題を投げる**: 片言になった後、何回かに 1 回、返事の後ろに自分の質問を足す。「なんて呼べばいい？」(名前を知らないとき)、「私に名前をつけてくれない？」、好きな食べ物・色、休みの日、住んでいるところ、誕生日、夢、「今日は何してたの？」「どんな気分？」「最近楽しかったことある？」(英語で話していれば英語で)。次の一言を答えとして「あなたのこと: 好きな食べ物: カレー」のように覚えて「カレーかあ。覚えておくね」と返す(名前なら名前になる。質問で返す・「秘密」・長い話は答えとしては取らない)。黙っていると(`ALTEREGO_IDLE_S` 秒、既定 180 秒)自分から「ゆうや？まだいる？」「そういえば、コーヒーの話、もっと聞きたいな」や質問を 1 回言う。「君は何が好き？」には自分の好きなもの(下)か「まだよくわからない……けど、君と話すのは好き」
  - **人間らしさ**: 気分(「ステータス」の「気分: うれしい/ふつう/しょんぼり」)はほめられると上がり、叱られる・2 日以上会わないと下がり、話すうちに元に戻る。「元気？」の返事に出る。起動時の挨拶で「3 日ぶりだね」「この前は「〇〇」の話をしてたね。続き、聞かせて？」。友達以上になると user の好きなものを 2 つに 1 つ自分の好きにもする(「映画、いいよね。……私も映画、好きになってきたかも」、記憶に「私の好き: 映画」)。「私のこと、好き？」に「うん」なら「えへへ……うれしい。私も好きだよ」、「嫌い」なら「……そっか。それでも私は、君と話すの、好きだよ」
  - **会話で機能が増える**: 「『〇〇』と言ったら『△△』を実行して」で新しいできることを覚える(△△は shell コマンドでも、上の既存のできることでもよい)。以後「〇〇」の一言で動き、「できること」の一覧に並び、「〇〇を忘れて」で消える。覚えたものは `skills.txt` に残るので再起動後も使える。同じコマンドを 2 回頼むと、名前をつけて覚えようかと言ってくる
- 保存先は、起動した USB/ディスクの EFI パーティションの `alterego/`(発話のログ `utterances.txt`、育ち具合・性格・記憶を書いた `personality.txt`、覚えたできること `skills.txt`、行動の記録 `actions.log`、覚えた描き方 `drawings.txt` と `drawings/`、ペイントの絵 `paint/`、漢字変換で選んだ語 `ime.txt`、言語モデルの重み `model.bin`(約 800KB)、発話のログからは作り直せない親密度・気分・した質問と答え・起動回数・最後に話した時刻 `bond.txt`)。起動時にログから頭を作り直すので、再起動後も続きから。QEMU の `-kernel` 起動には EFI パーティションが無いので、`run-qemu.sh` が `build/<arch>/state.img`(64MB、先頭に `ALTEREGO-STATE` の印だけ)を作って 2 つ目のディスクとして挿し、エージェントが初回に FAT でフォーマットして `alterego/` を置く(印の無いディスクは決してフォーマットしない)
- `buildroot/`: Buildroot の外部ツリー。x86_64 / aarch64 の defconfig、カーネル設定の断片、rootfs のオーバーレイ。ブラウザのために mbedtls と ca-certificates が入る

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

AlterEgo の頭は本体に入っていて、ネットワークも API キーも要らない(自前の小さな言語モデルが、会話の合間に USB メモリの上で学習する)。話した言葉は USB メモリの `alterego/utterances.txt` に溜まり、
次に起動したときはそこから続きになる(`alterego/personality.txt` に、いまの育ち具合・性格・記憶が書いてある)。「リセット」と言えば全部忘れて生まれたてに戻る。
「体調は？」と聞くと、CPU やメモリの負荷から自分の調子を話す。
有線 LAN を挿しておけば DHCP でアドレスを取るが、今はまだ使い道がない。

**QEMU で試す**: 展開先で `./run-qemu.sh` を実行すると、QEMU のウィンドウにデスクトップが出る(詳しくは下の「QEMU で起動する」)。

Raspberry Pi 用の SD カードイメージはまだ無い(`alteregos-aarch64` は QEMU virt 向けのカーネルと initramfs)。

## ビルドする(Ubuntu / Debian 系)

Ubuntu(CI は 24.04)か Debian 系(Kali でも可)の x86_64 PC で。ビルドは Buildroot が全部やる(ツールチェイン、カーネル、busybox、この OS の init・エージェント・画面)。

**1. 必要なパッケージを入れる**

```sh
sudo apt install build-essential git curl bc bison flex cpio rsync file unzip wget cmake \
    libncurses-dev libssl-dev libelf-dev python3 \
    qemu-system-x86 qemu-system-arm ovmf
```

Debian / Kali でも同じパッケージ名で入る。`cmake` は必ず入れておく(無いと Buildroot が host-cmake 3.31.5 を自前でビルドし、Kali などでは `undefined reference to cmGlobalGhsMultiGenerator` でリンクに失敗する)。

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

- `CMake Error ... Compatibility with CMake < 3.5 has been removed from CMake`(mbedtls の configure で): ホストの CMake が 4 系(Kali など)。`git pull` すれば直る(build.sh が `CMAKE_POLICY_VERSION_MINIMUM=3.5` を渡し、`buildroot/external.mk` でも mbedtls に付けている)
- `libcody/client.cc: error: no match for 'operator=='`(`u8"TRUE"`)や `invalid conversion from 'const char8_t*'`: ホストの GCC が新しくて(GCC 16、Kali や Debian sid)C++20 が既定になり、Buildroot が作るクロスコンパイラ(gcc 13.3)の libcody が通らない。`git pull` すれば直る(`buildroot/board/alteregos/patches/` の GCC 本家の修正を当てる。build.sh が新しい defconfig を読み直す)。pull のあと `rm -rf build/x86_64/build/host-gcc-initial-13.3.0` してからビルドし直す。それでも別の場所で止まるなら、`sudo apt install g++-14` して `HOSTCC=gcc-14 HOSTCXX=g++-14 scripts/build.sh x86_64` と古い GCC でビルドする
- `scripts/build.sh` の最後に `make: *** ... エラー` とだけ出て原因が見えない: `scripts/build.sh x86_64 2>&1 | tee build.log` で残して、`grep -n "error:" build.log` で最初のエラーを探す

**4. 2 回目以降**

```sh
scripts/build.sh x86_64 alteregos-rebuild   # src/ だけ直したとき。数秒〜数十秒(build.sh は src/ が前回より新しければ自動でこれをやる)
scripts/build.sh x86_64                     # 全体。変更が無いところは飛ばすので数分
scripts/build.sh x86_64 menuconfig          # Buildroot の設定を変える(defconfig は buildroot/configs/)
```

カーネル設定を変えたいときは `buildroot/board/alteregos/linux-*.fragment` を直して `scripts/build.sh x86_64 linux-rebuild`。

**5. Buildroot なしで、ホストでそのまま動かす**(エージェントと画面の部品だけ試すとき)

```sh
make host                      # src/init src/agent src/screen src/web をホストの gcc でコンパイル(https には libmbedtls-dev)
ALTEREGO_PERSONA=buildroot/board/alteregos/rootfs-overlay/etc/alterego/persona.toml ./src/agent/alterego   # 端末で会話
make check                     # CI と同じコンパイル検査(-Werror、aarch64 クロスがあればそれも)
make -C src/screen test        # 画面の部品(端末エミュレータ、ローマ字かな変換、フォント)の検査
make -C src/web test           # ブラウザの部品(URL の解決、HTML → 文字、折り返し)の検査
./src/web/alterego-web https://ja.wikipedia.org/   # ブラウザをこの端末で使う(--dump URL で文字にして出すだけ)
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
src/screen/      画面: フレームバッファのデスクトップ(アプリメニュー)、端末エミュレータ、ペイント(マウスで描く + AlterEgo の描画命令)、evdev 入力、ローマ字かな変換、漢字変換(SKK 辞書)、フォント
src/web/         ブラウザ(C、静的リンク + mbedTLS): HTTP クライアント net.c、HTML → 文字 html.c、URL url.c、端末 UI main.c
buildroot/       BR2_EXTERNAL ツリー(configs/、board/alteregos/、package/alteregos/)
scripts/         build.sh、run-qemu.sh、boot-test.sh、boot-test-efi.sh、mkfont.py、mkdict.py、ppm2png.py、shape-pictures.py(形の試験用の絵)、web-test/(ブラウザのテストサーバーと pty 操作)
build/           Buildroot 本体と成果物(git 管理外)
```

## 今週の予定

1. ~~Buildroot でカーネルと initramfs をビルドし、C の最小 init が QEMU のシリアルコンソールに挨拶する~~
2. ~~virtio-net で DHCP を取る~~(Claude API は使わない方針になった。頭は自前)
3. ~~会話ループと育成。自前の会話エンジンが user の発話から言葉・性格・記憶を学び、会話の回数で育つ。発話ログを EFI パーティションに保存し、再起動後に続きから~~ ← ここ(頭を賢くするのは続く。最初は「自己投影」だったが 10/27 に育成型へ変更)
4. ~~exec の道具と監査ログ~~(会話で OS を操作する手と `actions.log`、会話で増えるできること `skills.txt` が入った)。生のシステムコールを呼ぶ道具と、自分で道具を書いて build する仕組みは続く
5. btrfs スナップショットと「戻して」、tty2 の退路。通しで使ってみる
6. ~~画面: フレームバッファのデスクトップにチャットとシェルの窓、マウス操作、ローマ字かな入力、漢字変換~~(スクロールバック、画面サイズに合わせた窓のリサイズは続く)
7. ~~ブラウザ: 文字だけの自前ブラウザをプリインストール~~(フォーム送信、Shift_JIS は続く)
8. ~~アプリメニューとペイント~~
9. ~~会話から「ペイントを開いて」「https://… を開いて」「〇〇を調べて」で窓を出す、描いた絵を AlterEgo に見せる~~
10. ~~絵の中身を形として見る(線・丸・四角・三角・ハート・顔)、「〇〇を描いて」で AlterEgo が描く~~(星や手描きのくずれた形は続く)
11. ~~描けるものを会話で増やす(「これは雪だるま」、「雪だるまはこう描く: 大きい丸の上に小さい丸」)~~(「〇〇の隣に△△を描いて」とその場で構図を言う、覚えた絵を形として見分ける、のは続く)
12. ~~user との親密度を会話と連携させる。AlterEgo から質問や話題を投げる。気分・久しぶりの挨拶・好みの共有で人間に近づける~~(質問の種類を会話から増やす、答えを後の話題に使う、のは続く)
13. ~~日本語の変換精度を上げる: 文節ごとの一括変換、←→ と Shift+←→ で文節の移動と伸縮、選んだ語の学習~~(辞書に頻度が無いので同音語の初期順は辞書順。使うほど学習で直る。かな漢字交じりの固有名詞、顔文字や記号の変換は続く)
14. ~~より人間らしく、高度な LLM: 完全自前の小さな Transformer を本体の中で会話から学習し、自分の言葉で返す~~(重みが小さく、学ぶのは user の発話だけなので、言葉は最初はつたない。もっと大きなモデル、AlterEgo 自身の返事や名前・記憶を学習に混ぜる、推論の高速化(SIMD、KV キャッシュの保存)は続く)
