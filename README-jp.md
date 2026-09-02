# kobox2

`kobox2`は、sandbox化したLinux module runtimeを起動・監督するための、小さく
host非依存なcontrollerです。

## 実行時のイメージ図

```text
PachaOS gpud + kobox2 controller
              |
              | IPC / shared memory / capability transfer
              v
      linux-sandbox GPU process
```


## checkout

```sh
git clone --recurse-submodules https://github.com/kamedaga/kobox2.git
```

現在はrepository bootstrap段階です。安定版controller APIとwire ABIはまだ
確定していません。
