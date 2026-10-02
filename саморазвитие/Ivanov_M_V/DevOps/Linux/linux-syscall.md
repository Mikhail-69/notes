# Системные вызовы Linux (syscalls)

> Конспект по механизму системных вызовов в ядре Linux: от запроса
> пользовательской программы до `sysretq`. Основан на главе
> *System calls in the Linux kernel* (linux-insides, части 1–6).

---

## 📌 Оглавление

- [Что такое syscall](#что-такое-syscall)
- [Как ядро находит обработчик](#как-ядро-находит-обработчик)
- [Инициализация точки входа](#инициализация-точки-входа)
- [Путь вызова: entry_SYSCALL_64](#путь-вызова-entry_syscall_64)
- [Выход из syscall](#выход-из-syscall)
- [Реализация write](#реализация-write)
- [vsyscall и vDSO](#vsyscall-и-vdso)
- [Запуск программы: execve](#запуск-программы-execve)
- [Разбор open](#разбор-open)
- [Лимиты ресурсов: rlimit](#лимиты-ресурсов-rlimit)
- [Памятка](#памятка)

---

## Что такое syscall

**System call** — это запрос пользовательской программы к сервису ядра.
Читать и писать файлы, слушать сокет, создавать каталоги, завершаться —
всё это возможно только через syscall: код из Ring 3 физически не имеет
доступа к дискам и оборудованию.

По сути syscall — обычная функция на C в пространстве ядра, которую
вызывает userspace. Набор свой у каждой архитектуры: у `x86_64` 322
вызова, у `x86` — 358. Номера лежат в таблице
`arch/x86/entry/syscalls/syscall_64.tbl`.

Минимальный пример на ассемблере:

```assembly
_start:
    movq $1, %rax        # номер syscall: write
    movq $1, %rdi        # аргумент 1: fd = 1 (stdout)
    movq $msg, %rsi      # аргумент 2: указатель на строку
    movq $len, %rdx      # аргумент 3: длина
    syscall
```

Инструкция `syscall` на x86_64 делает одно: загружает `RIP` из регистра
`IA32_LSTAR` (прежний адрес кладётся в `RCX`). То есть прыжок идёт туда,
куда ядро записало адрес своего обработчика при старте:

```c
wrmsrl(MSR_LSTAR, entry_SYSCALL_64);
```

Какой именно обработчик вызывать, `syscall` не знает: он берёт **номер**
из регистра `%rax`, а адрес функции ищет в таблице `sys_call_table`.

**Раскладка регистров** (соглашение x86-64 calling conventions,
System V ABI):

| Роль | Регистр |
| --- | --- |
| Номер syscall | `rax` |
| Аргументы 1–6 | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` |
| При входе в обработчик | `rcx` = возврат, `r11` = флаги |

Аргументов больше шести — остаток кладётся на стек.

В libc есть `fopen`, `printf`, `fclose`, но в ядре их нет — есть `open`,
`write`, `close`. Функции libc это обёртки: они проверяют параметры,
потому что сам syscall должен быть предельно быстрым. Разница видна по
`strace` (syscall) и `ltrace` (вызовы libc).

Выполняемый прямо сейчас вызов процесса виден в procfs:
`cat /proc/1/syscall` (первое число — номер, `232` это `epoll_wait`).

---

## Как ядро находит обработчик

syscall реализован как программное прерывание: инструкция `syscall`
порождает исключение, управление переходит в код ядра. Таблица
вызовов — массив `sys_call_table` в `arch/x86/entry/syscall_64.c`:

```c
asmlinkage const sys_call_ptr_t
    sys_call_table[__NR_syscall_max+1] = {
    [0 ... __NR_syscall_max] = &sys_ni_syscall,
    #include <asm/syscalls_64.h>
};
```

**Тип.** `sys_call_ptr_t` — это `typedef` на указатель функции без
аргументов и возврата: `typedef void (*sys_call_ptr_t)(void);`

**Заполнение.** Сначала *все* слоты указывают на `sys_ni_syscall` —
заглушку для нереализованных вызовов, возвращающую `-ENOSYS`
(*Function not implemented*). Затем `asm/syscalls_64.h` (его генерирует
скрипт `syscalltbl.sh` из `.tbl`) перезаписывает нужные индексы
через designator'ы GCC:

```c
#define __SYSCALL_64(nr, sym, compat) [nr] = sym,
/* в итоге: [0] = sys_read, [1] = sys_write, [2] = sys_open, ... */
```

Для `x86_64` максимум — `547` (`__NR_syscall_max`, виден в
`include/generated/asm-offsets.h`).

---

## Инициализация точки входа

До того как обработчик можно зывать, надо записать адрес точки входа
в MSR. Это делает `syscall_init` (её вызывает `cpu_init` ← `trap_init`
при инициализации ядра):

```c
wrmsrl(MSR_STAR, ((u64)__USER32_CS)<<48 | ((u64)__KERNEL_CS)<<32);
wrmsrl(MSR_LSTAR, entry_SYSCALL_64);
```

| MSR | Назначение |
| --- | --- |
| `MSR_STAR` | `63:48` — сегмент кода пользователя для `sysret` |
| | `47:32` — сегмент ядра, база `CS`/`SS` при входе |
| `MSR_LSTAR` | Адрес точки входа `entry_SYSCALL_64` |
| `MSR_CSTAR` | Целевой `rip` для compatibility mode |
| `MSR_IA32_SYSENTER_*` | Параметры инструкции `sysenter` |
| `MSR_SYSCALL_MASK` | Флаги, **сбрасываемые** при входе в syscall |

При включённом `CONFIG_IA32_EMULATION` (32-битные программы под
64-битным ядром) в `MSR_CSTAR` пишется `entry_SYSCALL_compat`, иначе —
`ignore_sysret`, который просто возвращает `-ENOSYS`.

Маскируемые при входе флаги: `TF`, `DF`, `IF`, `IOPL`, `AC`, `NT`.

---

## Путь вызова: entry_SYSCALL_64

`entry_SYSCALL_64` в `arch/x86/entry/entry_64.S` выполняет всю
подготовку. Пошагово:

**1. Переключение на стек ядра.**

```assembly
SWAPGS_UNSAFE_STACK          # раскрывается в swapgs
movq %rsp, PER_CPU_VAR(rsp_scratch)
movq PER_CPU_VAR(cpu_current_top_of_stack), %rsp
```

`swapgs` меняет базу `GS` на kernel-значение из `MSR_KERNEL_GS_BASE` —
мы «переехали» на kernel stack. Пользовательский `%rsp` сохраняется в
per-cpu переменную `rsp_scratch`, сверху ставится вершина стека
текущего процессора.

**2. Сохранение пользовательского контекста.** На стек кладутся
пользовательские сегменты (`$__USER_DS`, `$__USER_CS`), старый `%rsp`,
флаги `%r11`, адрес возврата `%rcx` и все аргументы (`%rax`, `%rdi`,
`%rsi`, `%rdx`, `%r8`–`%r10`), плюс `-ENOSYS` как код ошибки на случай
нереализованного вызова. Регистры `rbp`, `rbx`, `r12`–`r15` не
сохраняются: по C ABI их обязан сохранять вызывающий
(callee-preserved).

**3. Проверка трейсинга.** Флаг `_TIF_SYSCALL_TRACE` (и соседние
`_TIF_SYSCALL_AUDIT`, `_TIF_SECCOMP`, `_TIF_SINGLESTEP`) в `thread_info`
ведёт в путь трейсинга, иначе сразу идём дальше.

**4. Проверка номера и вызов.**

```assembly
    cmpq $__NR_syscall_max, %rax   # есть ли такой номер
    ja 1f                          # нет → сразу на выход
    movq %r10, %rcx                # 4-й аргумент r10 → rcx (ABI)
    call *sys_call_table(, %rax, 8)  # индексация base + rax*8
```

`sys_call_table` — массив указателей по 8 байт, поэтому смещение
считается как `%rax * 8`. После `call` управление у `sys_read`,
`sys_write` или любого другого обработчика, объявленного через
`SYSCALL_DEFINE[N]`.

---

## Выход из syscall

Возврат — это ровно то же место в `entry_64.S`, строка после `call`:

```assembly
movq %rax, RAX(%rsp)   # результат обработчика → на стек
LOCKDEP_SYS_EXIT        # отладка блокировок (CONFIG_DEBUG_LOCK_ALLOC)

RESTORE_C_REGS_EXCEPT_RCX_R11
movq RIP(%rsp), %rcx    # адрес возврата в пользовательский код
movq EFLAGS(%rsp), %r11 # старые флаги
movq RSP(%rsp), %rsp    # старый стек

USERGS_SYSRET64
```

`USERGS_SYSRET64` — это `swapgs` (вернуть user GS) + `sysretq`
(возврат в userspace).

Цепочка целиком: `syscall` → процессор в режим ядра →
`entry_SYSCALL_64` (`swapgs`, kernel stack, сохранение регистров) →
проверка номера → `call` из `sys_call_table` → восстановление →
`swapgs` + `sysretq`.

---

## Реализация write

`write` объявлен в `fs/read_write.c` через макрос:

```c
SYSCALL_DEFINE3(write, unsigned int, fd,
        const char __user *, buf, size_t, count)
{
    struct fd f = fdget_pos(fd);
    ssize_t ret = -EBADF;

    if (f.file) {
        loff_t pos = file_pos_read(f.file);
        ret = vfs_write(f.file, buf, count, &pos);
        if (ret >= 0)
            file_pos_write(f.file, pos);
        fdput_pos(f);
    }

    return ret;
}
```

### Что делает SYSCALL_DEFINE3

Макрос определён в `include/linux/syscalls.h` и разворачивается в
цепочку `SYSCALL_METADATA` + `__SYSCALL_DEFINEx`. Первый работает
только при `CONFIG_FTRACE_SYSCALLS` (трейсер ловит вход/выход из
syscall), иначе раскрывается в пустую строку. Второй порождает пять
функций (`sys##name`, `SyS##name`, `SYSC##name` и др.), лишние — для
защиты от переполнения аргументов
([CVE-2009-0029](https://cve.mitre.org/cgi-bin/cvename.cgi?name=CVE-2009-0029)).
Итог — объявление `asmlinkage long sys_write(unsigned int fd,
const char __user *buf, size_t count)`.

### Тело функции

| Шаг | Функция | Что делает |
| --- | --- | --- |
| 1 | `fdget_pos(fd)` | Дескриптор → `struct fd` из `current->files` |
| 2 | `file_pos_read` | Возвращает `file->f_pos` — позицию в файле |
| 3 | `vfs_write` | Запись буфера в файл, область VFS |
| 4 | `file_pos_write` | Обновляет `f_pos`, если запись успешна |
| 5 | `fdput_pos(f)` | Снимает `f_pos_lock` — мьютекс позиции файла |

Атрибут `__user` у `buf` — метаданные для утилиты `sparse`: помечает
указатель, ведущий в пользовательскую память (зависит от `__CHECKER__`).

---

## vsyscall и vDSO

Вызов syscall дорог: надо прервать задачу, переключить контекст на
ядро, потом вернуться. Чтобы ускорить частые вызовы, придуманы два
механизма.

### Сравнение

| | `vsyscall` | `vDSO` |
| --- | --- | --- |
| Полное имя | virtual system call | virtual dynamic shared object |
| Расположение | страница `ffffffffff600000` | shared object в каждом процессе |
| В `ldd` | — | `linux-vdso.so.1` |
| Вызовов | 3: `gettimeofday`, `time`, `getcpu` | 4: те же + `clock_gettime` |
| Режим | `native` / `emulate` / `none` | всегда динамически |

### vsyscall

Ядро маппит страницу в userspace, и вызов выполняется прямо там — без
context switching. Маппинг делает `map_vsyscall`
(`arch/x86/entry/vsyscall/vsyscall_64.c`), вызывается из `setup_arch`.

Страница описана символом `__vsyscall_page`
(`arch/x86/entry/vsyscall/vsyscall_emu_64.S`), обработчики выровнены
по 1024 байта:

```assembly
__vsyscall_page:
    mov $__NR_gettimeofday, %rax
    syscall
    ret
    .balign 1024, 0xcc     # следующий обработчик через 1024 байта
    ...
```

Режим задаётся параметром командной строки `vsyscall=`
(`vsyscall_setup` → `early_param`):

| Режим | Права страницы | Поведение |
| --- | --- | --- |
| `native` | `__PAGE_KERNEL_RX \| _PAGE_USER` | Настоящие `syscall` |
| `emulate` | `__PAGE_KERNEL_RO \| _PAGE_USER` | Не исполняема → `#PF` |
| `none` | — | Страница не маппится |

Почему эмуляция: адреса фиксированы и всегда одинаковы — это
устаревший ABI и проблема безопасности.

При `emulate` обработчик `emulate_vsyscall` определяет номер вызова,
проверяет его и `access_ok`, затем диспетчеризуется по switch'у на
`sys_gettimeofday` / `sys_time` / `sys_getcpu`, кладёт результат в
`ax` и эмулирует `ret` (`regs->ip = caller`, `regs->sp += 8`).

glibc знает адреса наизусть: `0xffffffffff600000` (`gettimeofday`),
`0xffffffffff600400` (`time`), `0xffffffffff600800` (`getcpu`).

### vDSO

`vDSO` устраняет главный недостаток `vsyscall`: страница грузится
динамически и у каждого процесса свой адрес. Все программы,
слинкованные с glibc, используют её автоматически — её видно в выводе
`ldd` как `linux-vdso.so.1` и в `/proc/<pid>/maps` как `[vdso]`.

Инициализация — `init_vdso` (`arch/x86/entry/vdso/vma.c`),
регистрируется через `subsys_initcall`, инициализирует образ
`vdso_image_64` (а при `CONFIG_X86_X32_ABI` и `vdso_image_x32`).

`struct vdso_image` — образ для конкретного способа входа, его
генерирует программа `vdso2c` из разных исходников под `int 0x80`,
`sysenter`, `syscall`. Сами страницы маппятся при загрузке бинарника:
ядро зовёт `arch_setup_additional_pages` → `map_vdso(&vdso_image_64)`.

---

## Запуск программы: execve

Shell не запускает программы напрямую. Цепочка в bash:

```text
execute_command
--> execute_command_internal
----> execute_simple_command
------> execute_disk_command
--------> shell_execve
```

И всё заканчивается одним syscall:

```c
int execve(const char *filename,
           char *const argv[], char *const envp[]);
```

### Что делает ядро

`execve` (`fs/exec.c`) — тонкая обёртка над `do_execve` →
`do_execveat_common`. Первый аргумент `AT_FDCWD` означает: путь
интерпретируется относительно текущего рабочего каталога.

Последовательность внутри `do_execveat_common`:

| Шаг | Вызов | Назначение |
| --- | --- | --- |
| 1 | проверка `filename` | отбой при `NULL` и при превышении `RLIMIT_NPROC` |
| 2 | `unshare_files` | убрать утечку file descriptor'ов у бинарника |
| 3 | `kzalloc(sizeof(*bprm))` | выделить `struct linux_binprm` |
| 4 | `prepare_bprm_creds` | инициализация `cred` (uid/gid, security context) |
| 5 | `do_open_execat` | найти файл, проверить точку монтирования на `noexec` |
| 6 | `sched_exec()` | переехать на наименее загруженное CPU |
| 7 | `bprm_mm_init` | инициализировать `mm_struct` + временный стек |
| 8 | `count()` | посчитать `argc`/`envc` (лимит `MAX_ARG_STRINGS`) |
| 9 | `prepare_binprm` | взять `uid` из inode, прочитать первые 128 байт файла |
| 10 | `copy_strings*` | скопировать имя, `argv`, `envp` в стек |
| 11 | `exec_binprm` | выбрать обработчик формата |

`struct linux_binprm` (`include/linux/binfmts.h`) хранит всё нужное для
загрузки: `vma` (область памяти будущей программы), `mm` (дескриптор
памяти), вершину стека, `cred`, `argc`/`envc`, `filename`, `interp` и
буфер `buf[128]`.

### Определение формата

`search_binary_handler` идёт по списку зарегистрированных форматов
и зовёт `fmt->load_binary(bprm)`. Каталог обработчиков:

| Обработчик | Формат |
| --- | --- |
| `binfmt_script` | скрипты с `#!` (shebang) |
| `binfmt_misc` | прочие форматы по runtime-конфигурации |
| `binfmt_elf` / `binfmt_elf_fdpic` | ELF и ELF FDPIC |
| `binfmt_aout` / `binfmt_flat` | a.out и flat |
| `binfmt_em86` | Intel ELF на машинах Alpha |

`load_elf_binary` сверяет магический номер (`ELFMAG`) из прочитанных
128 байт, тип файла (`ET_EXEC`/`ET_DYN`) и архитектуру
(`elf_check_arch`), после чего грузит program header table — сегменты.

Дальше: чтение программного
интерпретатора из секции `.interp` (`/lib64/ld-linux-x86-64.so.2` для
`x86_64`) и подключённых библиотек, маппинг `bss` и `brk`, настройка
стека. Финал — `start_thread(regs, elf_entry, bprm->p)`: заполняются
регистры нового потока (`ip`, `sp`, `cs`, `ss`, `flags =
X86_EFLAGS_IF`, сегменты `fs`/`es`/`ds`) и зовётся `force_iret()`.

> **Важно:** `execve` не возвращает управление вызывающему. Код,
> данные и сегменты процесса **перезаписываются** сегментами новой
> программы. Выход из неё будет уже через syscall `exit`.

---

## Разбор open

С userspace всё просто: `int fd = open("test", O_RDONLY);`
Возвращается file descriptor — уникальный номер процесса, связанный
с открытым файлом. Открытые дескрипторы видны в `/proc/<pid>/fd/`.

`fs/open.c`:

```c
SYSCALL_DEFINE3(open, const char __user *, filename,
        int, flags, umode_t, mode)
{
    if (force_o_largefile())
        flags |= O_LARGEFILE;
    return do_sys_open(AT_FDCWD, filename, flags, mode);
}
```

`force_o_largefile()` на 64-битных архитектурах всегда `true`
(`BITS_PER_LONG != 32`). На 32-битной системе `O_LARGEFILE` пришлось бы
выставлять вручную — он разрешает открывать файлы, чей размер не
помещается в 32-битный `off_t`.

### do_sys_open

`do_sys_open` (там же) проходит четыре шага:

1. `build_open_flags(flags, mode, &op)` — нормализовать флаги.
2. `getname(filename)` — скопировать имя в ядро.
3. `get_unused_fd_flags(flags)` — выделить свободный дескриптор.
4. `do_filp_open(dfd, tmp, &op)` — разрешить путь в `struct file`,
   после чего `fd_install(fd, f)` ставит `file` в таблицу дескрипторов
   (при ошибке дескриптор освобождается через `put_unused_fd`).

### build_open_flags: разбор флагов

Цель — заполнить структуру `struct open_flags` (`open_flag`, `mode`,
`acc_mode`, `intent`, `lookup_flags`).

**Режим доступа.** `ACC_MODE(x)` отбирает два младших бита
(`x & O_ACCMODE`, где `O_ACCMODE = 3`) и использует их как индекс
в таблице масок `"\004\002\006\006"` — так из `O_RDONLY`/`O_WRONLY`/
`O_RDWR` (0/1/2) получаются маски `MAY_READ` / `MAY_WRITE`.

**Таблица флагов:**

| Флаг | Эффект |
| --- | --- |
| `O_CREAT` / `O_TMPFILE` | Задают права, иначе `mode` игнорируется |
| `O_CLOEXEC` | Закрыть дескриптор при `execve` — защита от утечки |
| `O_SYNC` | Запись не возвращается, пока данные не уйдут на диск |
| `O_TMPFILE` | Только с `O_RDWR` или `O_WRONLY`, иначе `-EINVAL` |
| `O_PATH` | Дескриптор без открытия файла: только `dup`, `fcntl` |
| `O_TRUNC` | Обрезает существующий файл до 0, даёт `MAY_WRITE` |
| `O_APPEND` | Дозапись в конец, даёт `MAY_APPEND` |
| `O_DIRECTORY` | `LOOKUP_DIRECTORY` — открыть можно только каталог |
| `O_NOFOLLOW` | Без `LOOKUP_FOLLOW` — не переходить по symlink |
| `O_EXCL` | С `O_CREAT`: ошибка, если файл уже существует |

Поле `intent` описывает намерение (`LOOKUP_OPEN`, `LOOKUP_CREATE`,
`LOOKUP_EXCL`) и обнуляется при `O_PATH`.

### Реальное открытие

**`getname`** (`fs/namei.c`) → `getname_flags` → `strncpy_from_user`:
путь копируется из пользовательской памяти в пространство ядра.
Структура `filename` хранит `name` (путь в ядре), `uptr` (оригинальный
указатель из userspace), `aname` (из audit-контекста), `refcnt` и
`iname` (встроенное хранилище, если путь короче `PATH_MAX`).

**`get_unused_fd_flags(flags)`** — берёт таблицу файлов процесса,
диапазон `0` … `RLIMIT_NOFILE`, выделяет свободный номер и помечает
его занятым.

**`do_filp_open`** (`fs/namei.c`) — главный шаг: разрешить путь
в `struct file`. Инициализируется `nameidata` (мост к `inode`), затем
`path_openat` зовётся до трёх раз: сначала с `LOOKUP_RCU` (быстрый
RCU-режим), при `-ECHILD` — без флагов (обычный режим), при `-ESTALE`
— с `LOOKUP_REVAL` (встречается в NFS).

1. `get_empty_flip()` — выделить `file` (проверка, не превышен ли
   лимит открытых файлов).
2. Ветки `do_tmpfile` / `do_o_path` для `O_TMPFILE` и `O_PATH`.
3. `path_init` — точка начала обхода (корень `/` или `AT_CWD`).
4. Цикл `link_path_walk` → `do_last`:
   - `link_path_walk` — обход пути по компонентам: проверка прав,
     каждый компонент через `walk_component` (обновляет запись из
     `dcache` или спрашивает ФС).
   - `do_last` — по результату заполняет `file` для последнего
     компонента и зовёт `vfs_open`.
5. `vfs_open` вызывает `open` конкретной ФС
   (`file_operations.open`).

---

## Лимиты ресурсов: rlimit

Каждый процесс потребляет файлы, CPU-время, память — ресурсы не
бесконечны, и для них есть лимиты. Три syscall:

```c
int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
int prlimit(pid_t pid, int resource,
            const struct rlimit *new_limit,
            struct rlimit *old_limit);
```

`prlimit` — расширение первых двух: позволяет читать и менять лимиты
**чужого** процесса по PID. Именно его использует `ulimit` из bash
(`strace ulimit -s` покажет `prlimit64(0, RLIMIT_STACK, ...)`).

### Два уровня лимита

| Уровень | Поле | Кто может менять |
| --- | --- | --- |
| `soft` | `rlim_cur` | любой процесс — текущий реальный лимит |
| `hard` | `rlim_max` | только superuser — потолок для `soft` |

`soft` никогда не превышает `hard`. Это структура `struct rlimit`
(`rlim_t rlim_cur` — soft, `rlim_t rlim_max` — hard).

### Типы ресурсов

| Ресурс | Ограничение |
| --- | --- |
| `RLIMIT_CPU` | процессорное время в секундах |
| `RLIMIT_FSIZE` | максимальный размер создаваемого файла |
| `RLIMIT_DATA` | максимальный размер data-сегмента |
| `RLIMIT_STACK` | максимальный размер стека процесса в байтах |
| `RLIMIT_CORE` | максимальный размер core-файла |
| `RLIMIT_RSS` | байты, которые можно занять в RAM |
| `RLIMIT_NPROC` | максимальное число процессов пользователя |
| `RLIMIT_NOFILE` | максимальное число открытых дескрипторов |
| `RLIMIT_MEMLOCK` | байты, которые можно заблокировать через `mlock` |
| `RLIMIT_AS` | максимальный размер виртуальной памяти |
| `RLIMIT_LOCKS` | число `flock`/`fcntl`-блокировок |
| `RLIMIT_SIGPENDING` | число ожидающих сигналов пользователя |
| `RLIMIT_MSGQUEUE` | байты для POSIX message queues |
| `RLIMIT_NICE` | максимальное значение `nice` |
| `RLIMIT_RTPRIO` | максимальный реалтайм-приоритет |
| `RLIMIT_RTTIME` | микросекунды под RT-политикой без блокирующих syscall |

Практика — читать лимиты при старте сервиса: systemd снимает ограничение
на coredump (`setrlimit(RLIMIT_CORE, RLIM_INFINITY)`), haproxy проверяет,
хватит ли дескрипторов под `maxconn` (`getrlimit(RLIMIT_NOFILE)`).

### Реализация в ядре

Все три syscall сводятся к `do_prlimit` (`kernel/sys.c`):
`getrlimit` только копирует результат в userspace через
`copy_to_user`, `setrlimit` забирает новую структуру через
`copy_from_user`.

Проверки в `do_prlimit`: валидность ресурса (`resource >= RLIM_NLIMITS`
→ `-EINVAL`), `soft <= hard` (иначе `-EINVAL`), а для `RLIMIT_NOFILE`
ещё `hard <= sysctl_nr_open` (иначе `-EPERM`, потолок читается из
`/proc/sys/fs/nr_open`). После проверок берётся
`read_lock(&tasklist_lock)` — обновление лимитов чужой задачи не
должно пересечься с работой signal-обработчиков. Затем
`tsk->signal->rlim + resource` даёт нужный `struct rlimit` из массива
лимитов процесса, и значения копируются в обе стороны: `new_rlim` →
настоящий лимит, настоящий → `old_rlim`.

---

## Памятка

| Вопрос | Ответ |
| --- | --- |
| Что такое syscall | Функция в ядре, дверь в стене Ring 3 |
| Где номер вызова | `%rax`, таблица — `syscall_64.tbl` |
| Где адрес обработчика | `MSR_LSTAR` → `entry_SYSCALL_64` |
| Индексация таблицы | `sys_call_table(, %rax, 8)` |
| Неизвестный номер | `sys_ni_syscall` → `-ENOSYS` |
| Вход в ядро | `swapgs` → kernel stack → push регистров → `call` |
| Выход из ядра | `swapgs` + `sysretq` |
| libc vs syscall | `fopen`/`printf` — обёртки над `open`/`write` |
| vDSO vs vsyscall | Динамический shared object vs фикс. страница |
| Что делает `execve` | Перезаписывает образ процесса, **не возвращается** |
| Как определяется формат | `search_binary_handler` → `binfmt_*` по магии |
| `O_CLOEXEC` | Закрыть дескриптор при `execve` — защита от утечки |
| `O_PATH` | Дескриптор без открытия файла |
| `soft` / `hard` | Текущий лимит / потолок (меняет только root) |
| Проверить syscall | `strace`, `/proc/PID/syscall`, `/proc/PID/fd/` |

---

> **Инструменты:** `strace` (syscall), `ltrace` (вызовы libc),
> `/proc/<pid>/syscall`, `/proc/<pid>/fd/`, `/proc/sys/fs/nr_open`.
>
> *Связанные конспекты: [Прерывания Linux](linux-interrupts.md),
> [Инициализация ядра и структуры данных](linux-init-data-structures.md),
> [Управление памятью и сборка](linux-mm-build-link.md),
> [CPU и cgroups](linux-cpu-cgroups.md).*
