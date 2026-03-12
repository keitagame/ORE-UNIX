# MyOS – Unix互換カーネル

実機にインストールして動作するUnix互換カーネルです。
gcc/bashなどのプログラムを動作させることができます。

---

## アーキテクチャ概要

```
myos/
├── boot/           ブートローダ連携 (Multiboot2, GDT, IDT)
│   ├── boot.S      アセンブリエントリ、ページテーブル設定、ISRスタブ
│   └── gdt_idt.c   GDT/IDT/TSS初期化、PIC再マップ
├── kernel/         カーネルコア
│   ├── kmain.c     カーネルエントリ、Multiboot2解析、初期化シーケンス
│   ├── process.c   task_struct、CFSスケジューラ、fork/exec/exit/waitpid
│   ├── elf.c       ELF32ローダ、ユーザー空間へのジャンプ
│   ├── syscall.c   int 0x80ディスパッチャ (Linux i386 ABI互換)
│   └── kprintf.c   カーネルprintf、panic、文字列ライブラリ
├── mm/             メモリ管理
│   ├── pmm.c       ビットマップ物理ページアロケータ
│   ├── vmm.c       仮想メモリ、ページテーブル、VMA、ページフォルト
│   └── kmalloc.c   カーネルヒープ + スラブアロケータ
├── fs/             ファイルシステム
│   ├── vfs.c       VFSコア：パス解決、マウント、syscallレイヤー
│   ├── ramfs.c     インメモリfs (initrd/tmpfs)
│   ├── pipe.c      匿名パイプ
│   └── procfs.c    /proc仮想ファイルシステム
├── drivers/        デバイスドライバ
│   ├── serial.c    16550 UART (コンソールI/O、割り込み駆動)
│   ├── timer.c     8254 PITタイマー (100Hz)
│   ├── ata.c       ATA PIOディスクドライバ
│   ├── keyboard.c  PS/2キーボード + VGAテキストモード
│   └── dev.c       デバイス番号レジストリ、/dev
└── include/
    ├── kernel/     カーネルヘッダ
    └── arch/x86/   x86アーキテクチャヘッダ
```

## 実装済み機能

### カーネル機能
- **Multiboot2** ブート対応 (GRUB2から起動)
- **高位ハーフカーネル** (0xC0000000、物理1MBにロード)
- **GDT/IDT/TSS** 完全セットアップ、リング0/3
- **PIC再マップ** (IRQ→32番台)
- **割り込み駆動I/O**

### メモリ管理
- ビットマップ物理ページアロケータ (〜1GBサポート)
- x86 4KBページング、カーネル/ユーザー分離
- コピーオンライト対応pgdir_clone (fork用)
- デマンドページング
- カーネルヒープ (フリーリスト + コアレッシング)
- スラブアロケータ (task_struct, inode等の固定サイズ)
- `brk()`, `mmap()`, `munmap()` システムコール

### プロセス管理
- `task_struct` (Linux互換構造体)
- ラウンドロビン + 優先度ベーススケジューラ
- `fork()` – アドレス空間の完全クローン
- `exec()` – ELF32ローダ、auxv、argv/envpスタック設定
- `exit()` / `waitpid()` – ゾンビ処理
- シグナル: `kill()`, `sigaction()`, SIG_DFL/SIG_IGN
- スリープ/ウェイクキュー

### ファイルシステム
- **VFS** – inode/dentry/file/super_block抽象化
- **ramfs/tmpfs** – インメモリfs、完全なPOSIX操作
- **procfs** – /proc/meminfo, cpuinfo, uptime, version, stat
- **パイプ** – 匿名パイプ (ブロッキング読み書き)
- **CPIO initrd** – newc形式、自動マウント・展開
- **マウントテーブル** – 複数fsのマウント/アンマウント

### システムコール (Linux i386 ABI互換)
全72個のシステムコールを実装:
read, write, open, close, fork, exec, exit, waitpid,
stat, lstat, fstat, lseek, dup, dup2, pipe, ioctl, fcntl,
mkdir, rmdir, unlink, link, rename, symlink, readlink,
chmod, chown, mknod, mount, umount, chdir, getcwd,
getdents, brk, mmap, munmap, mprotect, kill, sigaction,
getpid, getppid, getuid, setuid, geteuid, ... など

### デバイスドライバ
- **シリアル** (COM1, 115200 baud, 割り込み駆動)
- **PIT** (100Hz、スケジューラtick)
- **ATA PIO** (プライマリ/セカンダリ, LBA28/LBA48)
- **PS/2キーボード** (USキーマップ, Shift/Ctrl/CapsLock)
- **VGAテキスト** (80x25, スクロール, カーソル)
- **/dev/null, /dev/zero, /dev/full, /dev/random, /dev/urandom**
- **/dev/console, /dev/tty, /dev/ttyS0**
- **/dev/hda〜hdd**

---

## ビルド方法

### 必要ツール
```bash
# Ubuntu/Debian
sudo apt install gcc-i686-linux-gnu binutils-i686-linux-gnu \
                 qemu-system-x86 grub-pc-bin xorriso make

# またはソースからクロスコンパイラをビルド
# https://wiki.osdev.org/GCC_Cross-Compiler
```

### ビルド
```bash
make              # kernel.elfをビルド
make iso          # ブータブルISOを作成
make loc          # 行数確認
```

### QEMU実行
```bash
# カーネルのみ（initrdなし）
make qemu

# initrdあり（推奨）
make initrd       # initrdステージング準備
make qemu-initrd  # 実行

# ISOから起動
make qemu-iso

# GDBデバッグ
make qemu-gdb
# 別ターミナルで:
# i686-elf-gdb build/kernel.elf -ex 'target remote :1234'
```

---

## 実機インストール (x86 PC)

### USBブートディスク作成
```bash
# ISOを作成
make iso

# USBに書き込み (/dev/sdXを実際のUSBデバイスに変更)
sudo dd if=build/myos.iso of=/dev/sdX bs=4M status=progress
```

### GRUBエントリ (既存Linuxへのデュアルブート)
`/etc/grub.d/40_custom` に追加:
```
menuentry "MyOS" {
    insmod multiboot2
    multiboot2 (hd0,gpt1)/boot/kernel.elf
    module2    (hd0,gpt1)/boot/initrd.cpio initrd
    boot
}
```

### initrdにBusyboxを組み込む
```bash
# i686向け静的リンクbusyboxをビルド
git clone https://git.busybox.net/busybox
cd busybox
make ARCH=i386 CROSS_COMPILE=i686-linux-gnu- defconfig
# menuconfig で "Build static binary" を有効にする
make ARCH=i386 CROSS_COMPILE=i686-linux-gnu-

# MyOS initrdに組み込む
cp busybox ../myos/build/initrd_staging/bin/busybox
cd ../myos/build/initrd_staging
for cmd in sh ls cat echo cp mv rm mkdir mount umount \
           ps kill dmesg env pwd; do
  ln -sf busybox bin/$cmd
done

cd ../..
make initrd
```

### musl-libc + GCCを動かす (上級)
```bash
# musl-libc i386をビルド
./configure --target=i386-linux-musl --prefix=/usr/local/musl-i386
make && make install

# GCCをmuslでビルドしてinitrdに含める
# (詳細: https://musl.libc.org/how.html)
```

---

## 拡張方法

### 新しいシステムコールの追加
1. `include/kernel/process.h` に宣言を追加
2. `kernel/syscall.c` の `syscall_handler()` にケースを追加
3. 実装関数を適切なファイルに記述

### 新しいファイルシステムの追加
```c
// myfs.c
struct filesystem_type myfs_type = {
    .name  = "myfs",
    .mount = myfs_mount,
};
// kmain.cで:
register_filesystem(&myfs_type);
vfs_mount("/dev/hda1", "/mnt", "myfs", NULL);
```

### 新しいデバイスドライバの追加
```c
// drivers/mydev.c
register_chrdev(MY_MAJOR, "mydev", &mydev_ops);
// /devにノードを作成:
vfs_mknod("/dev/mydev", S_IFCHR|0666, MKDEV(MY_MAJOR, 0));
```

---

## 設計の特徴

- **Linux i386 ABI互換** – 静的リンクのLinuxバイナリが動作
- **Higher-half kernel** – ユーザー空間と完全分離
- **VFS抽象化** – inode/dentry/fileキャッシュ設計
- **スラブアロケータ** – 頻繁に使うオブジェクトの高速割り当て
- **CFS風スケジューラ** – 優先度付きラウンドロビン
- **モジュラー設計** – fsとdriversは独立して追加可能
