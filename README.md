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
- `src/agent/`: エージェント。persona.toml を読んで挨拶し、Claude API(libcurl + mbedtls、静的リンク)で会話する。1 往復ごとに安いモデルで振り返り、「わたし像」(性格・口癖・価値観)を書き直して保存する。文末の口癖も数える。キーが無ければ入力をそのまま返す
- 保存先は、起動した USB/ディスクの EFI パーティションの `alterego/`(API キーと `self_model.txt`)。QEMU の `-kernel` 起動では保存先が無く、その回限り
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

**Claude API のキーを渡す**(会話に必要。無ければ入力をそのまま返すだけになる):

```sh
scripts/set-api-key.sh alteregos-x86_64.img sk-ant-...     # USB に書く前にイメージへ入れる
```

書き込んだあとの USB メモリなら、PC でマウントして `alterego/api_key` というファイルにキーを書いても同じ。
起動後に AlterEgo が聞いてきたときに貼り付けてもよく、その場合も同じ場所に保存される。
有線 LAN を挿しておけば DHCP でアドレスを取る(Wi-Fi はまだ)。

AlterEgo は 1 往復ごとに「わたし」の性格・口癖・価値観を振り返って「わたし像」を書き直し、同じ場所の `alterego/self_model.txt` に保存する。
次に起動したときはそこから続きになる。

**QEMU で試す**(同梱の `run-qemu.sh` は展開先でそのまま動く。`ALTEREGO_API_KEY=sk-ant-... ./run-qemu.sh` で端末の中、`./run-qemu.sh x86_64 --efi` で UEFI 起動。後者はキーを `set-api-key.sh` でイメージに入れておく):

```sh
sudo apt install qemu-system-x86 ovmf
qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max -m 1024 \
    -bios /usr/share/ovmf/OVMF.fd -drive file=alteregos-x86_64.img,format=raw,if=virtio   # 実機と同じ起動(ウィンドウに表示)
qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max -m 1024 -nographic \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -kernel bzImage -initrd rootfs.cpio.gz -append "console=ttyS0 alterego.key=sk-ant-..."   # 端末の中で会話(Ctrl-A, X で終了)
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
scripts/         build.sh、run-qemu.sh、set-api-key.sh、boot-test.sh、boot-test-efi.sh
build/           Buildroot 本体と成果物(git 管理外)
```

## 今週の予定

1. ~~Buildroot でカーネルと initramfs をビルドし、C の最小 init が QEMU のシリアルコンソールに挨拶する~~
2. ~~virtio-net で DHCP を取り、Claude API に HTTPS で往復する~~
3. ~~会話ループと自己投影。1 往復ごとに「わたし」の性格・口癖・価値観を振り返って「わたし像」を書き直し、次の返事の人格に混ぜる。再起動後に続きから話せる(EFI パーティションに保存)~~ ← ここ(会話の履歴そのものの保存と SQLite はまだ)
4. syscall と exec の道具と監査ログ
5. btrfs スナップショットと「戻して」、tty2 の退路。通しで使ってみる
