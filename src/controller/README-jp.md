# controller実装

このディレクトリにはhost非依存なlifecycle state machineを置きます。public controller
APIとMIT wire protocolだけに依存し、Linuxや特定のhost OSには依存させません。

library契約は[`docs/controller-library-jp.md`](../../docs/controller-library-jp.md)で
定義します。
