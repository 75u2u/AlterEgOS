# AlterEgOS

Linux カーネルの上を全部作り直した、対話するための OS。
PID 1 がエージェント「AlterEgo」を起動し、bash も systemd もデスクトップもない。
話しかける相手が、そのまま OS。

設計の全体像(コンセプト、学習の仕組み、人格、権限、ロードマップ)は
[設計提案ドキュメント](https://claude.ai/artifact/9Le34Uw1axii9dFN7kokBd) にある。

## 決めていること

- カーネルは Linux LTS をそのまま使い、その上(init、シェル、サービス、UI)を自作する
- エージェントは root で動き、システムコールを直接呼ぶ。確認は求めず、全操作を記録して btrfs スナップショットで巻き戻す
- エージェントには人格がある。デフォルト名は AlterEgo(アルターエゴ)、丁寧な二十歳くらいの女性。これは出発点で、持ち主(呼び名は「わたし」)と話すたびに性格と口癖を学び、自分に投影して「わたし」に近づいていく(自己投影)
- 実装は C/C++(いずれ Rust 化)。開発は QEMU、移植先は PC(x86_64)と Raspberry Pi(aarch64)

## いまの状態(MVP 3 日目まで)

- `src/init/`: PID 1。擬似 FS のマウント、ホスト名と lo、eth0 と DHCP クライアント(busybox udhcpc)、エージェントと tty2 シェルの起動と再起動、シャットダウン
- `src/agent/`: エージェント。頭は外部のモデルや API に頼らない自前のもの(`brain.c`)。「わたし」の発話から文字 n-gram(1〜3 文字の文脈)と語彙・文末の統計を学び、似た発話を検索して「そのとき次に言ったこと」を返したり、n-gram で「わたし」の文体の文を生成したりする。最初は丁寧な自分の声で聞き役、30 回(persona.toml の `mirror_after`)聞くころには「わたし」の話し方になる(自己投影)。「わたし像」と聞くと覚えたことを話す
- 保存先は、起動した USB/ディスクの EFI パーティションの `alterego/`(発話のログ `utterances.txt` と `self_model.txt`)。起動時にログから頭を作り直すので、再起動後も続きから。QEMU の `-kernel` 起動では保存先が無く、その回限り
- `buildroot/`: Buildroot の外部ツリー。x86_64 / aarch64 の defconfig、カーネル設定の断片、rootfs のオーバーレイ

## 使ってみる(ビルドせずに)

[Releases](https://github.com/75u2u/AlterEgOS/releases) から `alteregos-x86_64-<tag>.tar.gz` を落として展開する。

**PC に USB メモリから起動する**(UEFI、Secure Boot はオフにする):

```sh
sudo dd if=alteregos-x86_64.img of=/dev/sdX bs=4M status=progress conv=fsync   # sdX は USB メモリ
```

USB から起動すると、画面に AlterEgo の挨拶が出て、キーボードで話しかけられる。
ブートローダはなく、カーネル自身が EFI アプリとして起動する(initramfs 埋め込み)。
Alt+F2 で tty2 の busybox シェル、Alt+F1 で AlterEgo に戻る。

AlterEgo の頭は本体に入っていて、ネットワークも API キーも要らない。話した言葉は USB メモリの `alterego/utterances.txt` に溜まり、
次に起動したときはそこから続きになる(`alterego/self_model.txt` に、いま覚えていることが書いてある)。
有線 LAN を挿しておけば DHCP でアドレスを取るが、今はまだ使い道がない。

**QEMU で試す**(同梱の `run-qemu.sh` は展開先でそのまま動く。`./run-qemu.sh` で端末の中、`./run-qemu.sh x86_64 --efi` で UEFI 起動。後者は覚えたことがイメージに残る):

```sh
sudo apt install qemu-system-x86 ovmf
qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max -m 1024 \
    -bios /usr/share/ovmf/OVMF.fd -drive file=alteregos-x86_64.img,format=raw,if=virtio   # 実機と同じ起動(ウィンドウに表示)
qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max -m 1024 -nographic \
    -kernel bzImage -initrd rootfs.cpio.gz -append "console=ttyS0"                            # 端末の中で会話(Ctrl-A, X で終了)
```

Raspberry Pi 用の SD カードイメージはまだ無い(`alteregos-aarch64` は QEMU virt 向けのカーネルと initramfs)。

## ビルド(Ubuntu)

```sh
sudo apt install build-essential git curl bc bison flex cpio rsync file unzip \
    libncurses-dev libssl-dev libelf-dev python3 qemu-system-x86 qemu-system-arm

scripts/build.sh x86_64        # 初回は 30〜60 分(ツールチェインとカーネル)
scripts/run-qemu.sh x86_64     # 終了は Ctrl-A, X
```

aarch64 は引数を `aarch64` に変えるだけ。`src/` だけ直したあとは
`scripts/build.sh x86_64 alteregos-rebuild` で数秒。

Buildroot なしで init とエージェントをコンパイルして動かすだけなら:

```sh
make host
ALTEREGO_PERSONA=buildroot/board/alteregos/rootfs-overlay/etc/alterego/persona.toml ./src/agent/alterego
```

## GitHub でのビルドと配布

- `ci`: push と PR のたびに、init とエージェントをホストと aarch64 クロスでコンパイルし、エージェントのスモークテストを回す(数分)
- `image`: `buildroot/` `src/` `scripts/` に触る push / PR と手動実行で、Buildroot の実ビルド、QEMU での起動テスト(シリアルに init とAlterEgo の挨拶が出るか)、イメージの Artifact 化を行う。初回は 1 アーキテクチャあたり 1 時間前後、2 回目以降はダウンロードと ccache のキャッシュで数分〜十数分
- `v*` のタグを push すると、同じイメージが GitHub Release に `alteregos-<arch>-<tag>.tar.gz` として出る

```sh
git tag v0.1.0 && git push origin v0.1.0     # Release を作る
```

Artifact / Release の中身: カーネル(`bzImage` または `Image`)、`rootfs.cpio.gz`、x86_64 は USB 用の `alteregos-x86_64.img`、`run-qemu.sh`、`VERSION`、`SHA256SUMS`。
x86_64 は `image` ワークフローの中で、QEMU の `-kernel` 起動に加えて OVMF(UEFI ファームウェア)から `.img` を起動するテストも通してから出している。

## レイアウト

```
src/init/        PID 1(C、静的リンク)
src/agent/       エージェント(C、静的リンク)
buildroot/       BR2_EXTERNAL ツリー(configs/、board/alteregos/、package/alteregos/)
scripts/         build.sh、run-qemu.sh、boot-test.sh、boot-test-efi.sh
build/           Buildroot 本体と成果物(git 管理外)
```

## 今週の予定

1. ~~Buildroot でカーネルと initramfs をビルドし、C の最小 init が QEMU のシリアルコンソールに挨拶する~~
2. ~~virtio-net で DHCP を取る~~(Claude API は使わない方針になった。頭は自前)
3. ~~会話ループと自己投影。自前の会話エンジンが「わたし」の発話から学び、話すほど「わたし」の話し方になる。発話ログを EFI パーティションに保存し、再起動後に続きから~~ ← ここ(エンジンを賢くするのは続く)
4. syscall と exec の道具と監査ログ
5. btrfs スナップショットと「戻して」、tty2 の退路。通しで使ってみる
