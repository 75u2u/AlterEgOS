# AlterEgOS

Linux カーネルの上を全部作り直した、対話するための OS。
PID 1 がエージェント「AlterEgo」を起動し、bash も systemd もデスクトップもない。
話しかける相手が、そのまま OS。

設計の全体像(コンセプト、学習の仕組み、人格、権限、ロードマップ)は
[設計提案ドキュメント](https://claude.ai/artifact/9Le34Uw1axii9dFN7kokBd) にある。

## 決めていること

- カーネルは Linux LTS をそのまま使い、その上(init、シェル、サービス、UI)を自作する
- エージェントは root で動き、システムコールを直接呼ぶ。確認は求めず、全操作を記録して btrfs スナップショットで巻き戻す
- エージェントには人格がある。デフォルト名は AlterEgo(アルターエゴ)、丁寧な二十歳くらいの女性
- 実装は C/C++(いずれ Rust 化)。開発は QEMU、移植先は PC(x86_64)と Raspberry Pi(aarch64)

## いまの状態(MVP 1 日目)

- `src/init/`: PID 1。擬似 FS のマウント、ホスト名と lo、エージェントと tty2 シェルの起動と再起動、シャットダウン
- `src/agent/`: エージェントの土台。persona.toml を読んで挨拶し、入力を返す(会話は 3 日目から)
- `buildroot/`: Buildroot の外部ツリー。x86_64 / aarch64 の defconfig、カーネル設定の断片、rootfs のオーバーレイ

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

Artifact / Release の中身: カーネル(`bzImage` または `Image`)、`rootfs.cpio.gz`、`run-qemu.sh`、`VERSION`、`SHA256SUMS`。
手元で試すなら展開して `scripts/run-qemu.sh` と同じ引数で QEMU を起動する。

## レイアウト

```
src/init/        PID 1(C、静的リンク)
src/agent/       エージェント(C、静的リンク)
buildroot/       BR2_EXTERNAL ツリー(configs/、board/alteregos/、package/alteregos/)
scripts/         build.sh、run-qemu.sh、boot-test.sh
build/           Buildroot 本体と成果物(git 管理外)
```

## 今週の予定

1. ~~Buildroot でカーネルと initramfs をビルドし、C の最小 init が QEMU のシリアルコンソールに挨拶する~~ ← ここ
2. virtio-net で DHCP を取り、init から Claude API に HTTPS で 1 往復する
3. 会話ループと persona.toml。会話を SQLite に保存し、再起動後に続きから話せる
4. syscall と exec の道具と監査ログ
5. btrfs スナップショットと「戻して」、tty2 の退路。通しで使ってみる
