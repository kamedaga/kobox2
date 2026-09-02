# kobox2

`kobox2` is a small, host-independent controller for launching and supervising
sandboxed Linux module runtimes.

## Runtime overview

```text
PachaOS gpud + kobox2 controller
              |
              | IPC / shared memory / capability transfer
              v
      linux-sandbox GPU process
```

## Checkout

```sh
git clone --recurse-submodules https://github.com/kamedaga/kobox2.git
```

The project is currently at the repository-bootstrap stage; there is no stable
controller or wire ABI yet.
