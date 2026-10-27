# Инициализация ядра Linux и структуры данных

> Конспект по главам книги `linux-insides`
> (`linux-initialization-1`–`10`, `linux-datastructures-1`–`3`).
> Только суть: ключевые функции, структуры, таблицы и команды.
> Начало пути (до `start_kernel`) — в конспекте
> [здесь](linux-boot-deep-dive.md).

---

## 📌 Оглавление

- [Первые шаги в ядре и таблицы страниц](#первые-шаги-в-ядре-и-таблицы-страниц)
- [Обработка прерываний и IDT](#обработка-прерываний-и-idt)
- [Точка входа start_kernel](#точка-входа-start_kernel)
- [Архитектурная инициализация setup_arch](#архитектурная-инициализация-setup_arch)
- [Планировщик, zonelists и PID hash](#планировщик-zonelists-и-pid-hash)
- [RCU и целочисленные ID](#rcu-и-целочисленные-id)
- [Финал: кеши, procfs и первый init](#финал-кеши-procfs-и-первый-init)
- [Двусвязный список](#двусвязный-список)
- [Radix tree](#radix-tree)
- [Битовые массивы и операции](#битовые-массивы-и-операции)
- [Шпаргалка команд и функций](#шпаргалка-команд-и-функций)

---

## Первые шаги в ядре и таблицы страниц

После распаковки ядра загрузчик выполняет `jmp *%rax`, где `rax` — адрес
точки входа `startup_64` в `arch/x86/kernel/head_64.S` (секция
`.head.text`). Базовый физический адрес ядра — `0x1000000`, виртуальный —
`0xffffffff81000000`.

### Функция `__startup_64`

Определена в `arch/x86/kernel/head64.c`, получает физический адрес и
`boot_params`. Вычисляет `load_delta` — смещение между скомпилированным и
реальным размещением ядра (**kASLR**):

```c
load_delta = physaddr - (unsigned long)(_text - __START_KERNEL_map);
```

Ключевые шаги:

- проверка выравнивания `_text` по 2 МБ (`PMD_SHIFT = 21`), иначе —
  бесконечный цикл;
- `fixup_pointer(ptr, physaddr)` переводит виртуальные адреса статических
  таблиц страниц в физические;
- при включённом SME — `sme_enable(bp)`, `sme_encrypt_kernel(bp)`;
- исправление записей `pmd` текста/данных ядра на `load_delta`
  (вместе с `cleanup_highmap`);
- `phys_base += load_delta` (должен совпадать с первой записью
  `level2_kernel_pgt`).

### Статические таблицы страниц

Описаны в `head_64.S`:

```c
early_top_pgt[511]        -> level3_kernel_pgt
level3_kernel_pgt[510]    -> level2_kernel_pgt   /* 512 МБ ядра */
level3_kernel_pgt[511]    -> level2_fixmap_pgt
level2_fixmap_pgt[506]    -> level1_fixmap_pgt   /* vsyscalls */
```

- `early_top_pgt` — 511 нулевых записей + запись `level3_kernel_pgt`
  (при `CONFIG_PAGE_TABLE_ISOLATION` на одну запись больше);
- `level2_kernel_pgt` макросом `PMDS` отображает 512 МБ под `.text`;
- флаги записей: `_KERNPG_TABLE_NOENC` (без `_PAGE_USER`) и
  `_PAGE_TABLE_NOENC` (с `_PAGE_USER`).

### Отображение 1:1 (identity mapping)

Динамические таблицы берутся из массива `early_dynamic_pgts`
(`EARLY_DYNAMIC_PAGE_TABLES = 64`):

```c
i = (physaddr >> PGDIR_SHIFT) % PTRS_PER_PGD;
pgd[i] = (pgdval_t)pud + pgtable_flags;
```

Записи PMD заполняются на весь размер ядра
(`DIV_ROUND_UP(_end - _text, PMD_SIZE)`), при исчерпании буфера таблиц —
`reset_early_page_tables()` и повтор.

### Последнее приготовление (асемблер)

- включение PAE и PGE, загрузка `cr3` адресом `early_top_pgt`;
- `cpuid`/`rdmsr` по `MSR_EFER`: включение `_EFER_SCE`
  (инструкции `SYSCALL`/`SYSRET`) и при поддержке — `_EFER_NX`;
- биты `CR0`: `PE` (защищённый режим), `MP`, `ET`, `NE`, `WP`
  (запрет записи в read-only из ring 0), `AM`, `PG` (paging);
- стек: `initial_stack = init_thread_union + THREAD_SIZE`;
  `THREAD_SIZE = 16 КБ` (4 страницы) на x86_64;
- `lgdt early_gdt_descr` — **GDT** на `GDT_ENTRIES = 32` дескриптора,
  лежит в per-CPU переменной `gdt_page` (одна страница на каждый CPU);
- сегментные регистры зануляются, `MSR_GS_BASE` (`0xc0000101`) получает
  адрес irqstack;
- переход в C: `lretq` → `x86_64_start_kernel(real_mode_data)`.

### Таблицы страниц в `x86_64_start_kernel`

- `BUILD_BUG_ON(...)` — статические проверки адресов ядра и модулей
  (ошибка компиляции при ложном условии);
- `reset_early_page_tables()` — обнуление `early_top_pgt`, запись его
  физического адреса в `cr3`;
- очистка `_bss`, подготовка `init_top_pgt`;
- `copy_bootdata(__va(real_mode_data))` — копирование `boot_params`
  и командной строки (`__va(x) = x + PAGE_OFFSET`,
  `PAGE_OFFSET = 0xffff880000000000`);
- `sanitize_boot_params` зануляет незаполненные загрузчиком поля;
- `clear_page(init_level4_pgt)` обнуляет корневую таблицу
  (файл `arch/x86/lib/clear_page_64.S`, блоки по 64 байта);
  `init_level4_pgt` отображает первые 2 ГБ (identity) и 512 МБ ядра;
- `reserve_ebda_region()` — резерв блока `EBDA` (адрес берётся из
  `0x40E`, сдвиг на 4; порог `INSANE_CUTOFF = 128 КБ`) через
  `memblock_reserve` между низкой памятью и 1 МБ.

### Первое знакомство с memblock

`memblock` — ранний аллокатор физической памяти (`mm/block.c`),
всё в секции `.meminit.data`:

```c
struct memblock {
    bool bottom_up;
    phys_addr_t current_limit;
    struct memblock_type memory;
    struct memblock_type reserved;
};
```

`struct memblock_region` описывает регион: `base`, `size`, `flags`, `nid`
(NUMA-узел). Добавление диапазона — `memblock_add_range`, резервирование —
`memblock_reserve(base, size)`. Отладка — параметр командной строки
`memblock=debug`. Далее вызывается `start_kernel()`.

---

## Обработка прерываний и IDT

### Теория

Три типа прерываний:

- **программные** — обычно syscall;
- **аппаратные** — например, нажатие клавиши;
- **исключения** — генерируются процессором (деление на ноль,
  недоступная страница).

Каждому прерыванию присваивается номер вектора `0..255`: первые `32` —
исключения (`NUM_EXCEPTION_VECTORS = 32`), остальные — пользовательские
прерывания. Исключения делятся на типы:

- **faults** — после обработки команда повторяется;
- **traps** — состояние сохранено после вызвавшей команды;
- **aborts** — состояния нет, возврата к месту исключения нет.

### Исключения x86 (векторы 0–21)

| Вектор | Мнемоника | Описание | Тип | Код ошибки |
| --- | --- | --- | --- | --- |
| 0 | `#DE` | Деление на ноль | Ошибка | Нет |
| 1 | `#DB` | Отладка | Ошибка | Нет |
| 2 | --- | Немаскабельное (NMI) | Прерыв. | Нет |
| 3 | `#BP` | Точка останова `INT 3` | Ловушка | Нет |
| 4 | `#OF` | Переполнение `INTO` | Ловушка | Нет |
| 5 | `#BR` | Выход за границы `BOUND` | Ошибка | Нет |
| 6 | `#UD` | Неверный опкод | Ошибка | Нет |
| 7 | `#NM` | Устройство недоступно | Ошибка | Нет |
| 8 | `#DF` | Двойная ошибка | Авария | Да |
| 10 | `#TS` | Неверный TSS | Ошибка | Да |
| 11 | `#NP` | Сегмент отсутствует | Ошибка | Нет |
| 12 | `#SS` | Ошибка сегмента стека | Ошибка | Да |
| 13 | `#GP` | Общее нарушение защиты | Ошибка | Да |
| 14 | `#PF` | Ошибка страницы | Ошибка | Да |
| 16 | `#MF` | Ошибка x87 FPU | Ошибка | Нет |
| 17 | `#AC` | Проверка выравнивания | Ошибка | Да |
| 18 | `#MC` | Проверка машины | Авария | Нет |
| 19 | `#XM` | Исключение SIMD (SSE) | Ошибка | Нет |
| 20 | `#VE` | Исключение виртуализации | Ошибка | Нет |

### Устройство IDT

**IDT** — массив записей-шлюзов (gates), как GDT, но записи называются
шлюзами. В 32-битном режиме дескриптор 8 байт, в 64-битном — **16 байт**
(вектор умножается на 16). База хранится в `IDTR`, загрузка — `lidt`.

Поля записи IDT (64-битный режим): смещение обработчика (64 бита,
разбито на три части), `P` (присутствие), `DPL` (уровень привилегий),
тип (дескриптор задачи/прерывания/ловушки), `IST` (переключение на
выделенный стек), селектор сегмента.

Отличие interrupt gate от trap gate: interrupt gate очищает флаг `IF`
(вложенные прерывания блокируются), `IF` возвращается через `iret`.

При прерывании CPU сохраняет в стек `RFLAGS`, `CS`, `RIP`; если у
исключения есть код ошибки — и его. Выход — `iret`.

### Заполнение и загрузка IDT

```c
for (i = 0; i < NUM_EXCEPTION_VECTORS; i++)
    set_intr_gate(i, early_idt_handler_array[i]);
```

- `set_intr_gate` / `idt_setup_from_table` (`arch/x86/kernel/idt.c`)
  заполняют `idt_table`, максимум — `BUG_ON(n > 0xFF)`;
- загрузка: `load_idt(&idt_descr)`, где
  `idt_descr.size = IDT_ENTRIES * 16 - 1`;
- отдельная таблица `trace_idt_table` — для контрольных точек.

### Начальные обработчики

`early_idt_handler_array` (288 байт, шаг `EARLY_IDT_HANDLER_SIZE = 9`):
на каждый вектор — 2 байта (подстановка фиктивного кода ошибки, если его
нет), 2 байта `push $номер` и 5 байт `jmp early_idt_handler_common`.

```text
| %rflags | %cs | %rip | код ошибки | номер вектора | <-- %rsp
```

`early_idt_handler_common` в `head_64.S`:

- защита от рекурсии (`early_recursion_flag`);
- сохранение всех регистров общего назначения;
- для вектора 14 (`#PF`) — передача `cr2` в `rdi` и вызов
  `early_make_pgtable`, для остальных — `%rsp` (указатель на `pt_regs`)
  и `early_fixup_exception`;
- возврат через `iretq`.

### Обработка ошибки страницы

`early_make_pgtable(address)` создаёт записи таблиц на лету: ищет запись
PGD в `early_top_pgt`, спускается по уровням `p4d/pud/pmd`
(пятиуровневый paging отключён — `p4d = pgd`), при невалидной записи
выделяет таблицу из `early_dynamic_pgts`.

### Остальные обработчики

`early_fixup_exception(regs, trapnr)` (`arch/x86/mm/extable.c`):

- NMI и зацикливание (`early_recursion_flag > 2`) — игнорируются/стоп;
- `fixup_exception` ищет адрес в секции `__ex_table`
  (`search_exception_tables`);
- `fixup_bug` обрабатывает `#UD`: при `BUG_TRAP_TYPE_WARN` пропускает
  `UD2` (`regs->ip += LEN_UD2`).

---

## Точка входа start_kernel

`start_kernel` (`init/main.c`) — точка входа архитектурно-независимого
кода ядра, содержит около 86 вызовов функций. Цель — завершить
инициализацию и запустить процесс `init` с `PID = 1`.

### Атрибуты функций

```c
#define __init  __section(.init.text) __cold notrace
```

Код в секции `.init.text` освобождается функцией `free_initmem` после
инициализации. `notrace` — без вызовов профилирования.

### Начальная задача и стек

```c
struct task_struct init_task = INIT_TASK(init_task);
```

- начальное состояние `0` (runnable), флаг `PF_KTHREAD`;
- `union thread_union` объединяет `thread_info` и стек на `THREAD_SIZE`
  (16 КБ = 4 страницы, `thread_info` — внизу стека);
- с `v4.9` при `CONFIG_THREAD_INFO_IN_TASK` флаги `thread_info`
  переехали в `task_struct`;
- «канарейка» конца стека: `STACK_END_MAGIC = 0x57AC6E9D`
  (`set_task_stack_end_magic`).

Далее: `smp_setup_processor_id`, `debug_objects_early_init`,
`boot_init_stack_canary` (случайное число из энтропии + TSC в
`current->stack_canary`), `local_irq_disable` (инструкция `cli`).

### Первая активация процессора: `boot_cpu_init`

ID процессора читается через `this_cpu_read(cpu_number)` — ассемблерная
инструкция `movl %gs:cpu_number, %reg` (`gs` — база per-CPU области,
см. [конспект по per-cpu](linux-cpu-cgroups.md)). Далее boot CPU
регистрируется во всех масках сразу:

```c
set_cpu_online(cpu, true);
set_cpu_active(cpu, true);
set_cpu_present(cpu, true);
set_cpu_possible(cpu, true);
```

Затем — `pr_notice("%s", linux_banner)`, например:

```text
Linux version 4.0.0-rc6+ (alex@localhost) (gcc 4.9.1) #319 SMP
```

### Начало `setup_arch`

`setup_arch(char *command_line)` (`arch/x86/kernel/setup.c`) — большая
архитектурная инициализация. Первый шаг — резерв памяти под само ядро:

```c
memblock_reserve(__pa_symbol(_text),
                 (unsigned long)__bss_stop - (unsigned long)_text);
```

`__pa_symbol(x)` переводит символ в физический адрес
(`x - __START_KERNEL_map + phys_base`).

Резервирование initrd — `early_reserve_initrd`: адрес и размер берутся из
`boot_params.hdr.ramdisk_image` + старших 32 бит `ext_ramdisk_image`
(поле `0x218/4`, протокол 2.00+), затем
`memblock_reserve(ramdisk_image, ramdisk_end - ramdisk_image)`.

---

## Архитектурная инициализация setup_arch

### Тrap-гейты и отладка

`early_trap_init` (`arch/x86/kernel/traps.c`) инициализирует gate
отладки `#DB` (флаг `TF` в rflags) и `#BP` (`INT3`) и перезагружает
IDT:

```c
set_intr_gate_ist(X86_TRAP_DB, &debug, DEBUG_STACK);
set_system_intr_gate_ist(X86_TRAP_BP, &int3, DEBUG_STACK);
load_idt(&idt_descr);
```

- `IST` (Interrupt Stack Table) — до 7 выделенных стеков в TSS на CPU;
  `set_system_intr_gate_ist` ставит `dpl = 0x3` (доступно из user space);
- обработчик описан макросом в `entry_64.S`:

```assembly
idtentry debug do_debug has_error_code=0 paranoid=1 \
          shift_ist=DEBUG_STACK
```

- `paranoid=1` — переход на спецстек; приход из ядра проверяется по
  младшим битам `CS`, при необходимости выполняется `SWAPGS`;
- если кода ошибки нет — в стек пушится `-1 (однородный кадр);
- `120` байт (`ORIG_RAX-R15`) под регистры общего назначения;
- обработчик C — `do_debug(pt_regs, error_code)`.

### Ранний ioremap

`early_ioremap_init` (`arch/x86/mm/ioremap.c`) + `early_ioremap_setup`
готовят временные boot-time маппинги I/O и памяти: массив `slot_virt`
на `FIX_BTMAPS_SLOTS` слотов (`fixmap` — фиксированные виртуальные
адреса `FIXADDR_START`…`FIXADDR_TOP`), обнуление и подключение
таблицы `bm_pte` через `pmd_populate_kernel`.

### Корневое устройство и memory map

- `ROOT_DEV = old_decode_dev(boot_params.hdr.root_dev)` — старый формат
  16 бит (8 major + 8 minor); новый `new_decode_dev` — 12/20 бит;
  major определяет драйвер, minor — устройство;
- ресурсы описываются структурой `resource` (дерево
  `parent/sibling/child`) и видны в `/proc/iomem` и `/proc/ioports`;
  корень — `iomem_resource`, конец
  `(1ULL << boot_cpu_data.x86_phys_bits) - 1`;
- `setup_memory_map` копирует карту `e820` из BIOS, вывод в лог:

```text
e820: BIOS-provided physical RAM map:
BIOS-e820: [mem 0x0000000000000000-0x000000000009d7ff] usable
```

- `copy_edd` копирует данные BIOS EDD (MBR-сигнатуры, EDD-информацию)
  из `boot_params` в структуру `edd`;
- дескриптор памяти init-процесса `init_mm` заполняется границами
  `_text/_etext/_edata/_brk_end`; `mm_struct` хранит `mm_rb`
  (красно-чёрное дерево VMA), `pgd = swapper_pg_dir`, счётчики
  `mm_users`/`mm_count`, семафор `mmap_sem`;
- ресурсы `code/data/bss_resource` с физическими адресами видны
  в `/proc/iomem` как `Kernel code`, `Kernel data`, `Kernel bss`;
- `x86_configure_nx` включает `_PAGE_NX` в `__supported_pte_mask`,
  если CPU поддерживает NX-бит и он не отключён.

### Разбор ранних параметров командной строки

Параметры объявляются макросом `early_param(str, fn)` → структура
`obs_kernel_param { str, setup_func, early }` в секции `.init.setup`
между метками `__setup_start` и `__setup_end`:

```c
parse_early_param() -> parse_early_options() -> parse_args()
                    -> do_early_param()   /* early == 1 */
```

Примеры параметров: `noexec=on|off` (выводит `x86_report_nx`),
`pci=earlydump` (дамп конфигурации PCI регистров — обход
`bus:slot:func` = 256×32×8 через `read_pci_config`),
`memblock=debug`, `io_delay=...`.

Предупреждение `acpi_mps_check`: при `CONFIG_X86_LOCAL_APIC` без
`CONFIG_X86_MPPARSE` и переданных `acpi=off` / `acpi=noirq` /
`pci=noacpi` — APIC отключается (`setup_clear_cpu_cap(X86_FEATURE_APIC)`).

### Завершение разбора памяти (e820)

- `finish_e820_parsing` санирует карту (`sanitize_e820_map`);
- `e820_add_kernel_range` проверяет, что ядро помечено `E820RAM`;
- `trim_bios_range` резервирует первые 4096 байт;
- `max_pfn = e820_end_of_ram_pfn()` считается только по типам
  `E820_RAM` и `E820_PRAM` (`MAX_ARCH_PFN = 0x400000000` на x86_64);
- `max_low_pfn` — верхняя граница low memory (если RAM больше 4 ГБ —
  отдельный подсчёт), `high_memory = __va(max_pfn * PAGE_SIZE)`.

### DMI scanning

`dmi_scan_machine` (`drivers/firmware/dmi_scan.c`) сканирует физическую
память `0xF0000`–`0xFFFFF` (`0x10000` байт, шаг 16) в поисках
сигнатуры `_SM_` (SMBIOS/DMI); также доступен путь через EFI
configuration table. Результат в логе:

```text
SMBIOS 2.7 present.
DMI: Gigabyte Technology Co., Ltd. Z97X-UD5H-BK...
```

`dmi_memdev_walk` обходит модули памяти (выделение — `dmi_alloc` через
`RESERVE_BRK(..., 65536)`).

### Поиск SMP-конфигурации

`find_smp_config` сканирует регионы `0x0`, `639*0x400` и `0xF0000` на
MP floating pointer structure (`struct mpf_intel`): сигнатура
`SMP_MAGIC_IDENT`, `length == 1`, верный checksum, спецификация `1`
или `4`. При находке — `memblock_reserve` на структуру и таблицу
MP configuration table (спека MultiProcessor Specification).

### Дополнительные операции с памятью

- `early_alloc_pgt_buf` — буфер таблиц страниц (`INIT_PGT_BUF_SIZE`)
  в области `brk` (секция сразу после BSS);
- `reserve_brk` резервирует memblock под `brk` и зануляет
  `_brk_start` (больше аллокаций не будет);
- `memblock_set_current_limit(ISA_END_ADDRESS)` (= `0x100000`), затем
  `memblock_x86_fill` заполняет memblock по карте e820:

```text
MEMBLOCK configuration:
 memory size = 0x1fff7ec00 reserved size = 0x1e30000
 memory[0x0] [0x1000-0x9efff] 0x9e000 bytes flags: 0x0
 reserved[0x0] [0x9f000-0xfffff] 0x61000 bytes flags: 0x0
```

- `reserve_real_mode` — резерв низкой памяти под real mode trampoline;
- `trim_low_memory_range` — резерв первой страницы (IVT/BDA);
- `init_mem_mapping` — реконструкция direct mapping на `PAGE_OFFSET`;
- `early_trap_pf_init` — обработчик `#PF`; `setup_real_mode` —
  trampoline в режиме реального адреса.

### Лог-буфер и перенос initrd

- `setup_log_buf` — кольцевой буфер `printk`, размер
  `1 << CONFIG_LOG_BUF_SHIFT` (допустимо 12…21), выделение через
  `memblock_virt_alloc`;
- `reserve_initrd` переносит initrd в область direct mapping:
  если `ramdisk_size >= mapped_size/2` — `panic("initrd too large")`,
  иначе `relocate_initrd` ищет место через
  `memblock_find_in_range` (при неудаче — `panic`) и освобождает
  старый блок; вывод `RAMDISK: [mem 0x36d20000-0x37687fff]`.

### io_delay, DMA, sparse memory, vsyscall

Параметр `io_delay` (порт задержки ввода-вывода):

| Значение | Смысл |
| --- | --- |
| `0x80` | стандартная задержка через порт `0x80` |
| `0xed` | альтернативный порт `0xed` |
| `udelay` | простая задержка 2 мкс |
| `none` | без задержки |

- `dma_contiguous_reserve` — резерв непрерывной области для DMA
  (устройств, работающих с памятью без CPU); размер задаётся
  `cma=nn[MG]@[start[-end]]` или опциями `CONFIG_CMA_SIZE_SEL_*`;
- `paging_init` — инициализация **Sparsemem** (разбиение памяти на
  banks в NUMA): `sparse_init` + `zone_sizes_init`;
- `map_vsyscall` отображает страницу vsyscall
  (`ffffffffff600000` = `-10UL << 20`) через `__set_fixmap`;
  режим `EMULATE` (по умолчанию) / `NATIVE` / `NONE`;
  заготовки — `__vsyscall_page` (`gettimeofday`, `time`, `getcpu`):
  `mov $__NR_..., %rax; syscall; ret`;
- `get_smp_config` читает найденную MP-таблицу, затем
  `prefill_possible_map` заполняет `cpu_possible_mask`.

### Возврат в `start_kernel`

- `setup_command_line` создаёт три буфера: `saved_command_line`,
  `initcall_command_line` (для `do_initcall_level`) и
  `static_command_line` (для `parse_args`);
- `setup_nr_cpu_ids` — фактическое число CPU:

```c
nr_cpu_ids = find_last_bit(cpumask_bits(cpu_possible_mask),
                           NR_CPUS) + 1;
```

  `find_last_bit` обходит слова с конца и возвращает позицию последнего
  установленного бита через инструкцию `bsr` (`__fls`); `NR_CPUS` —
  предел из `CONFIG_NR_CPUS`, он может быть больше реального числа CPU.

---

## Планировщик, zonelists и PID hash

### Подготовка SMP

- `setup_per_cpu_areas` — создание per-CPU областей (подробно —
  в конспекте [по per-cpu](linux-cpu-cgroups.md));
- `smp_prepare_boot_cpu` → `native_smp_prepare_boot_cpu`:
  перезагрузка GDT (`switch_to_new_gdt`, `GDT_SIZE = 256`), запись
  базы irqstack и canary в `gs` (`load_percpu_segment`), затем

```c
cpumask_set_cpu(me, cpu_callout_mask);
per_cpu(cpu_state, me) = CPU_ONLINE;
```

- маски SMP-«дозвона»: bootstrap CPU выставляет бит следующего
  secondary CPU в `cpu_callout_mask`, тот инициализируется и ставит
  бит в `cpu_callin_mask`.

### Zonelists

`build_all_zonelists` (`mm/page_alloc.c`) задаёт порядок зон, в которых
будет идти выделение, если текущая зона не может удовлетворить запрос.

Физическая память делится на `nodes` (при поддержке NUMA; без неё —
один узел, см. `/sys/devices/system/node/node0/numastat`), каждый узел
описывается `struct pglist_data` (`pg_data_t`). Узлы делятся на зоны:

| Зона | Диапазон |
| --- | --- |
| `ZONE_DMA` | 0–16 МБ |
| `ZONE_DMA32` | DMA для 32-битных устройств (ниже 4 ГБ) |
| `ZONE_NORMAL` | весь RAM от 4 ГБ на x86_64 |
| `ZONE_HIGHMEM` | отсутствует на x86_64 |
| `ZONE_MOVABLE` | перемещаемые страницы |

Информация — `cat /proc/zoneinfo`.

### Подготовка перед планировщиком

- `page_alloc_init` — callback'и cpu hotplug состояния
  `CPUHP_PAGE_ALLOC_DEAD` (при `CONFIG_HOTPLUG_CPU`);
- повторный `parse_early_param` и `parse_args` (не все архитектуры
  вызывали разбор в `setup_arch`);
- `jump_label_init` — инициализация jump label;
- `pidhash_init` — хеш-таблица PID:

```c
static struct hlist_head *pid_hash;
pid_hash = alloc_large_system_hash("PID", sizeof(*pid_hash),
                                   0, 18, HASH_EARLY | HASH_SMALL,
                                   &pidhash_shift, NULL, 0, 4096);
```

  размер — от `2^4` до `2^12` элементов, `hlist` отличается от
  двусвязного списка одним указателем; вывод:

```text
PID hash table entries: 4096 (order: 3, 32768 bytes)
```

- `vfs_caches_init_early` — ранняя инициализация VFS;
- `sort_main_extable` — сортировка таблицы исключений ядра
  (участок `__start___ex_table`…`__stop___ex_table`);
- `trap_init` — обработчики ловушек;
- `mm_init` — запуск менеджера памяти:

```c
page_ext_init_flatmem();
mem_init();            /* освобождает bootmem */
kmem_cache_init();     /* кеши slab */
percpu_init_late();    /* per-CPU через slub */
pgtable_init();
vmalloc_init();
```

### Инициализация планировщика: `sched_init`

Файл — `kernel/sched/core.c`. Планировщик **CFS** (Completely Fair
Scheduler) моделирует идеальный процессор: каждая runnable-задача
получает `1/n` времени. Политики планирования:

| Политика | Тип | Назначение |
| --- | --- | --- |
| `SCHED_NORMAL` | обычная | большинство приложений (nice) |
| `SCHED_BATCH` | обычная | неинтерактивные задачи |
| `SCHED_IDLE` | обычная | только когда CPU свободен |
| `SCHED_FIFO` | real-time | без квантов, приоритет |
| `SCHED_RR` | real-time | квантованная круговая |

Политики инкапсулируют **scheduler classes**. Групповое планирование
(`CONFIG_FAIR_GROUP_SCHED`, `CONFIG_RT_GROUP_SCHED`) позволяет планировать
набор задач как одну; единица планирования — `sched_entity`, а не
напрямую `task_struct`.

Ключевые шаги `sched_init`:

- `sched_clock_init()` — `sched_clock_running = 1`;
- инициализация таблицы ожидания: `WAIT_TABLE_SIZE = 256` записей
  `bit_wait_table`;
- расчёт и выделение (`kzalloc`) памяти под `root_task_group`:
  массивы `se`/`cfs_rq` (CFS) и `rt_se`/`rt_rq` (real-time) на
  каждый `nr_cpu_ids` (по два — указатель на задачу и на run queue);
- полосы пропускания real-time/deadline:

```text
/proc/sys/kernel/sched_rt_period_us  = 1000000
/proc/sys/kernel/sched_rt_runtime_us = 950000
```

  (в cgroup настраиваются `<cgroup>/cpu.rt_period_us` и
  `cpu.rt_runtime_us`); для группы — `init_rt_bandwidth` и
  `init_dl_bandwidth`;

- `init_defrootdomain()` (при `CONFIG_SMP`) — **root domain**:
  структура `root_domain` отслеживает CPU, пригодные для push/pull
  real-time задач (борьба с bottleneck'ами при росте числа CPU);
- при `CONFIG_CGROUP_SCHED` — SLAB под `task_group`, списки
  `siblings`/`children` корневой группы и `autogroup_init`
  (автогруппы создаются при `setsid`);
- цикл `for_each_possible_cpu(i)` — инициализация run queue `rq`
  (`kernel/sched/sched.h`) для каждого возможного CPU;
- `set_load_weight(&init_task)` — вес задачи по `static_prio`
  (nice): поля приоритета `prio` (динамический), `static_prio`
  (начальный), `normal_prio` (от политики); idle-задаче — минимальный
  вес `WEIGHT_IDLEPRIO`;
- `init_sched_fair_class` регистрирует `SCHED_SOFTIRQ` с обработчиком
  `run_rebalance_domains` (ребалансировка run queue);
- финал: статистика планировщика и `scheduler_running = 1`.

---

## RCU и целочисленные ID

### Отключение вытеснения

После `sched_init` вызывается `preempt_disable`. **Preemption** —
способность ядра прервать текущую задачу ради более приоритетной;
на раннем boot есть только один init-процесс, и останавливать его
нельзя до `cpu_idle`.

```c
#define preempt_disable() \
do { preempt_count_inc(); barrier(); } while (0)
```

- счётчик — per-CPU переменная `DECLARE_PER_CPU(int, __preempt_count)`;
- `barrier()` — оптимизационный барьер: без него компилятор мог бы
  переставить `preempt_disable(); foo(); preempt_enable();` так, что
  `foo()` станет прерываемой;
- далее — проверка прерываний: `WARN(!irqs_disabled(), ...)`,
  при необходимости `local_irq_disable()` (`cli`).

### Целочисленные ID: `idr_init_cache`

Библиотека `idr` (`lib/idr.c`) назначает объектам целочисленные ID и
ищет объекты по ID:

```c
idr_layer_cache = kmem_cache_create("idr_layer_cache",
                    sizeof(struct idr_layer), 0, SLAB_PANIC, NULL);
```

Пример — подсистема i2c: `static DEFINE_IDR(i2c_adapter_idr);`,
номер шины выделяется `idr_alloc(&i2c_adapter_idr, adap, ...)`.

### Идея RCU

**RCU** (read-copy-update) — масштабируемый механизм синхронизации для
редко изменяемых структур: при изменении создаётся копия, все правки
идут в неё, остальные работают со старой версией; в безопасный момент
оригинал заменяется копией.

| Термин | Значение |
| --- | --- |
| critical section | чтение; вход — `rcu_read_lock`, выход — `rcu_read_unlock` |
| quiescent state | поток вне critical section |
| grace period | момент, когда все потоки в quiescent state |
| removal | атомарное удаление элемента без освобождения памяти |
| шаг 2 | после grace period — окончательное освобождение |

Варианты: classic RCU, **tree RCU** (`CONFIG_TREE_RCU`,
`kernel/rcu/tree.c`) и tiny RCU (`CONFIG_TINY_RCU` при `CONFIG_SMP=n`).

### `rcu_init` (`kernel/rcu/tree.c`)

```c
rcu_bootup_announce();          /* "Hierarchical RCU implementation." */
rcu_init_geometry();            /* число узлов дерева */
rcu_init_one(&rcu_bh_state, &rcu_bh_data);
rcu_init_one(&rcu_sched_state, &rcu_sched_data);
__rcu_init_preempt();           /* при CONFIG_PREEMPT_RCU */
open_softirq(RCU_SOFTIRQ, rcu_process_callbacks);
cpu_notifier(rcu_cpu_notify, 0);
pm_notifier(rcu_pm_notify, 0);
rcu_early_boot_tests();
```

Глобальное состояние — `struct rcu_state`, иерархия узлов:

```c
struct rcu_node node[NUM_RCU_NODES];
#define NUM_RCU_NODES (RCU_SUM - NR_CPUS)
```

Каждый `rcu_node` содержит lock для пары CPU; дерево покрывает все CPU
(малые системы — одним узлом). Пример на 8 CPU (один лист на 2 CPU):

```text
             +------------------+
             |   root rcu_node  |
             +--------+---------+
                |            |
        +-------v----+  +---v--------+
        |  rcu_node  |  |  rcu_node  |
        +--+------+--+  +--+------+-+
           |      |        |      |
     ... (ещё 4 rcu_node) ...
     CPU1/2   CPU3/4   CPU5/6   CPU7/8
```

`rcu_init_geometry`:

- считает jiffies до первого/следующего `fqs` (force-quiescent-state):
  `RCU_JIFFIES_TILL_FORCE_QS = 1 + (HZ > 250) + (HZ > 500)`,
  делитель `RCU_JIFFIES_FQS_DIV = 256` (по умолчанию переменные
  равны `ULONG_MAX`);
- выходит early, если `rcu_fanout_leaf` и `nr_cpu_ids` не менялись;
- ёмкость уровней: `rcu_capacity[0] = 1`,
  `rcu_capacity[1] = rcu_fanout_leaf`, далее умножение на
  `CONFIG_RCU_FANOUT`; цикл считает число `rcu_node` на каждом уровне.

Регистрация softirq: `open_softirq` кладёт обработчик в массив
`softirq_vec[]`; структура `softirq_action` содержит одно поле —
`action`. Просмотр в системе:

```text
$ cat /proc/softirqs
        CPU0    CPU1
  TIMER:  137779  108110
  RCU:     81290   68062
  SCHED: 102350   75950
```

### Продолжение `start_kernel` после RCU

- `trace_init` — подсистема tracing;
- `radix_tree_init` (`lib/radix-tree.c`) — см. раздел про radix tree;
- IRQ: `early_irq_init`, `init_IRQ`, `softirq_init`;
- таймеры: `init_timers`, `hrtimers_init`, `time_init`;
- `perf_event_init`, `profile_init`;
- `local_irq_enable()` (`sti`) и `kmem_cache_init_late`;
- `console_init` (`drivers/tty/tty_io.c`);
- `lockdep_info`, `debug_objects_mem_init`, `kmemleak_init`,
  `setup_per_cpu_pageset`, `numa_policy_init`, `sched_clock_init`,
  `pidmap_init`, `anon_vma_init`, `acpi_early_init`.

---

## Финал: кеши, procfs и первый init

### Небольшие кеши

- `init_espfix_bsp` (при `CONFIG_X86_ESPFIX64`) — не даёт утечь битам
  `31:16` регистра `esp` при возврате на 16-битный стек; ставит
  espfix PUD в `init_level4_pgt` (`ESPFIX_BASE_ADDR` — запись `-2`
  в PGD); далее `init_espfix_random` и `init_espfix_ap`;
- `thread_info_cache_init` — кеш `thread_info`, если
  `THREAD_SIZE (16384) >= PAGE_SIZE (4096)`;
- `cred_init` — кеш учётных данных (`uid`, `gid`, ...):

```c
cred_jar = kmem_cache_create("cred_jar", sizeof(struct cred),
                             0, SLAB_HWCACHE_ALIGN | SLAB_PANIC,
                             NULL);
```

### `fork_init` — кеш `task_struct`

- `task_struct_cachep` создаётся при отсутствии
  `CONFIG_ARCH_TASK_STRUCT_ALLOCATOR` (на x86_64 не компилируется);
- `arch_task_cache_init` — кеш `task_xstate` (состояние **FPU**)
  и `setup_xstate_comp` (смещения расширенных состояний xsave);
- максимум потоков: `MAX_THREADS = FUTEX_TID_MASK = 0x3fffffff`;
- лимиты ресурсов init-задачи:

```c
init_task.signal->rlim[RLIMIT_NPROC].rlim_cur = max_threads / 2;
init_task.signal->rlim[RLIMIT_SIGPENDING] =
        init_task.signal->rlim[RLIMIT_NPROC];
```

  `struct rlimit { rlim_cur; rlim_max; }`; видно в
  `cat /proc/self/limits` (`Max processes`, `Max pending signals`).

### `proc_caches_init` — кеши процессов

| Кеш | Управляет |
| --- | --- |
| `sighand_cachep` | обработчиками сигналов |
| `signal_cachep` | дескриптором сигналов |
| `files_cachep` | открытыми файлами |
| `fs_cachep` | информацией о ФС |
| `mm_cachep` | memory descriptor (`mm_struct`) |
| `vm_area_cachep` | участками виртуальной памяти |

`vm_area_cachep` создаётся макросом-обёрткой:

```c
vm_area_cachep = KMEM_CACHE(vm_area_struct, SLAB_PANIC);
```

`KMEM_CACHE` выравнивает слот по `__alignof__` структуры, в отличие от
явного значения в `kmem_cache_create`. Далее `mmap_init` и
`nsproxy_cache_init` (кеши под VMA и namespaces).

Остальное: `buffer_init` (`fs/buffer.c`) — кеш `buffer_head`,
лимит буферов = 10% `ZONE_NORMAL`;
`vfs_caches_init` — поздняя инициализация dcache/inode и хеш-таблиц
монтирований; `signals_init` — кеш `sigqueue` (очередь
real-time сигналов); `page_writeback_init` — соотношение «грязных»
(dirty) страниц.

### Корень procfs

`proc_root_init` (`fs/proc/root.c`):

```c
err = register_filesystem(&proc_fs_type);
```

- `proc_self_init` — inode для `/proc/self`;
- `proc_setup_thread_self` — `/proc/thread-self`;
- `proc_symlink("mounts", NULL, "self/mounts")`;
- каталоги: `sysvipc` (при `CONFIG_SYSVIPC`), `fs`, `driver`,
  `fs/nfsd`, `bus`, `tty`, `tty/ldisc`;
- `proc_sys_init` создаёт `/proc/sys` и инициализирует Sysctl.

Пропущенные (зависящие от конфигурации) вызовы `start_kernel`:
`taskstats_init_early`, `delayacct_init`, `security_init`,
`check_bugs`, `ftrace_init`, `cgroup_init`.

### `rest_init`: init и idle

`rest_init` (`init/main.c`) — последний вызов `start_kernel`:

```c
kernel_thread(kernel_init, NULL, CLONE_FS);
pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
```

- `kernel_thread` (через `clone`) создаёт kernel-поток с общим
  информационным блоком ФС: `PID = 1` — init (`kernel_init`),
  `PID = 2` — `kthreadd` (управляет созданием остальных
  kernel-потоков, виден в `ps -ef | grep kthreadd`);
- `task_struct` kthreadd находится под RCU-локом
  (`find_task_by_pid_ns`);
- синхронизация — completion:

```c
complete(&kthreadd_done);   /* в rest_init */
wait_for_completion(&kthreadd_done);   /* в kernel_init_freeable */
```

  Completions (`include/linux/completion.h`): объявление
  (`DECLARE_COMPLETION`) → ожидание (`wait_for_completion`) →
  сигнализация (`complete`);

- затем текущая задача становится idle и уходит в idle-цикл:

```c
init_idle_bootup_task(current);      /* idle_sched_class */
schedule_preempt_disabled();
cpu_startup_entry(CPUHP_ONLINE);     /* -> cpu_idle_loop, PID 0 */
```

  idle-класс выполняется только когда CPU больше нечего делать;
  `cpu_idle_loop` проверяет `need_resched()` в цикле.

### `kernel_init`: запуск первого процесса

`kernel_init_freeable` ждёт готовности kthreadd, затем:
`gfp_allowed_mask = __GFP_BITS_MASK`, `set_mems_allowed` (все CPU и
NUMA-узлы), `set_cpus_allowed_ptr`, pid для `cad` (Ctrl-Alt-Delete),
`smp_prepare_cpus`, ранние initcall — `do_pre_smp_initcalls`,
`smp_init`, `lockup_detector_init`, `sched_init_smp`.

Далее `do_basic_setup` («Now we can finally start doing some real
work..»): реинициализация cpuset, `khelper` (вызовы в user-space из
ядра), tmpfs, подсистема drivers, workqueue user-mode helper,
post-early вызов initcall. Открытие начальной консоли:

```c
if (sys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0)
    pr_err("Warning: unable to open an initial console.\n");
(void) sys_dup(0);
(void) sys_dup(0);
```

Путь ramdisk — `rdinit=` из командной строки или `/init` по умолчанию;
при отсутствии файла — `prepare_namespace()` (`init/do_mounts.c`)
монтирует initrd. После: `async_synchronize_full` (ждём асинхронные
вызовы), `free_initmem` (освобождаем `__init_begin`…`__init_end`),
`mark_rodata_ro` (защищаем `.rodata`), затем

```c
system_state = SYSTEM_RUNNING;
```

Запуск init по приоритету вариантов:

| Приоритет | Что запускается |
| --- | --- |
| 1 | `ramdisk_execute_command` (`rdinit=` или `/init`) |
| 2 | `execute_command` (параметр `init=` командной строки) |
| 3 | `/sbin/init`, `/etc/init`, `/bin/init`, `/bin/sh` |
| 4 | `panic("No working init found...")` |

`run_init_process` заполняет `argv_init = { "init", NULL }` и вызывает
`do_execve(...)`. На этом инициализация ядра завершена: путь от
`start_kernel` до первого `init` пройден полностью.

---

## Двусвязный список

Основная структура — `include/linux/types.h`:

```c
struct list_head {
    struct list_head *next, *prev;
};
```

Отличие от классических списков (например, `GList` из glib): узел **не
хранит указатель на данные**. Реализация — **intrusive list**: узел
содержит только указатели `next`/`prev`, а данные включают узел в себя.
Структура становится универсальной (не зависит от типа данных).

Пример включения:

```c
struct nmi_desc {
    spinlock_t lock;
    struct list_head head;
};
```

### Живой пример: misc-устройства

Драйверы с общим major `MISC_MAJOR = 10` (свой minor) регистрируются
в общем списке:

```c
static LIST_HEAD(misc_list);
#define LIST_HEAD(name) \
    struct list_head name = LIST_HEAD_INIT(name)
#define LIST_HEAD_INIT(name) { &(name), &(name) }
```

Пустой список указывает сам на себя (`INIT_LIST_HEAD`: `next = prev =
list`). Добавление устройства в `misc_register`:

```c
list_add(&misc->list, &misc_list);
```

`list_add` вставляет элемент сразу после `head` через `__list_add`:

```c
static inline void __list_add(struct list_head *new,
                              struct list_head *prev,
                              struct list_head *next)
{
    next->prev = new;
    new->next  = next;
    new->prev  = prev;
    prev->next = new;
}
```

### Обратный путь: `list_entry` и `container_of`

```c
#define list_entry(ptr, type, member) container_of(ptr, type, member)
```

Параметры: `ptr` — указатель на `list_head`, `type` — тип структуры,
`member` — имя поля со списком внутри структуры:

```c
const struct miscdevice *p = list_entry(v, struct miscdevice, list);
```

`container_of` (методическое выражение GNU C, значение — последнее
выражение в скобках):

```c
#define container_of(ptr, type, member) ({                   \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);     \
    (type *)( (char *)__mptr - offsetof(type, member) ); })
```

- первая строка — типобезопасность (проверяет наличие поля `member`);
- `((type *)0)->member` даёт смещение поля от начала структуры;
- `offsetof` — `((size_t) &((TYPE *)0)->MEMBER)`;
- итог: адрес структуры = адрес поля − смещение поля.

Остальное API `include/linux/list.h`: `list_add_tail`, `list_del`,
`list_replace`, `list_move`, `list_is_last`, `list_empty`,
`list_cut_position`, `list_splice`, `list_for_each`,
`list_for_each_entry` и др.

---

## Radix tree

Файлы: `include/linux/radix-tree.h` и `lib/radix-tree.c`.
Инициализируется функцией `radix_tree_init` из `start_kernel`.

**Radix tree** — это **сжатый trie** (compressed trie). Trie хранит в
узлах отдельные символы, ключ восстанавливается обходом от корня
(например, ключи `go` и `cat`); в сжатом trie узлы с единственным
потомком убраны. В ядре radix tree отображает **целочисленные ключи**
в значения.

### Структуры

```c
struct radix_tree_root {
    unsigned int            height;    /* высота дерева */
    gfp_t                   gfp_mask;  /* как выделять память */
    struct radix_tree_node __rcu *rnode;  /* дочерний узел */
};
```

Флаги выделения `gfp_mask`: `GFP_NOIO` — можно спать и ждать память,
`__GFP_HIGHMEM` — разрешена high memory, `GFP_ATOMIC` — высокий
приоритет, без сна.

Узел:

```c
struct radix_tree_node {
    unsigned int    path;    /* смещение в родителе + высота */
    unsigned int    count;   /* число дочерних узлов */
    union {
        struct { struct radix_tree_node *parent;
                 void *private_data; };
        struct rcu_head rcu_head;   /* освобождение узла */
    };
    struct list_head private_list;  /* для пользователя дерева */
    void __rcu *slots[RADIX_TREE_MAP_SIZE];
    unsigned long tags[RADIX_TREE_MAX_TAGS]
                      [RADIX_TREE_TAG_LONGS];
};
```

- `slots[]` — указатели на данные; пустой слот хранит `NULL`;
- `tags[]` — отдельные биты-метки на записях дерева.

### API

Инициализация двумя способами:

```c
RADIX_TREE(name, gfp_mask);          /* объявление + инициализация */
/* или вручную: */
struct radix_tree_root my_radix_tree;
INIT_RADIX_TREE(my_tree, gfp_mask);  /* height=0, rnode=NULL */
```

Операции:

| Функция | Параметры | Действие |
| --- | --- | --- |
| `radix_tree_insert` | root, index, data | вставка записи |
| `radix_tree_delete` | root, index | удаление записи |
| `radix_tree_lookup` | root, index | поиск по ключу |
| `radix_tree_gang_lookup` | root, results, first, max | пакетный поиск |
| `radix_tree_lookup_slot` | root, index | слот с данными |

```c
unsigned int radix_tree_gang_lookup(
        struct radix_tree_root *root, void **results,
        unsigned long first_index, unsigned int max_items);
```

возвращает число записей (не больше `max_items`), отсортированных
по ключам начиная с `first_index`.

---

## Битовые массивы и операции

Файлы: `lib/bitmap.c`, `include/linux/bitmap.h` и архитектурный
`arch/x86/include/asm/bitops.h`. Примеры использования: cpumask
(см. [конспект](linux-cpu-cgroups.md)), множество занятых IRQ при
инициализации.

### Объявление

```c
unsigned long my_bitmap[8];               /* простой способ */
#define DECLARE_BITMAP(name, bits) \
    unsigned long name[BITS_TO_LONGS(bits)]
```

`BITS_TO_LONGS(nr)` — округление вверх числа 8-байтовых элементов:

```c
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define BITS_TO_LONGS(nr) \
    DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))
```

Например, `DECLARE_BITMAP(my_bitmap, 64)` → `unsigned long my_bitmap[1]`.

### Архитектурные операции (x86)

У каждой операции две версии: **атомарная** и **неатомарная**
(с двумя подчёркиваниями). Атомарность гарантирует, что две операции
не выполняются над одними данными одновременно: на x86 это инструкции
`xchg`, `cmpxchg` и префикс `lock`.

| Версия | Установка | Снятие | Проверка | Инверсия |
| --- | --- | --- | --- | --- |
| неатомарная | `__set_bit` | `__clear_bit` | — | `__change_bit` |
| атомарная | `set_bit` | `clear_bit` | `test_bit` | `change_bit` |

Неатомарная установка — одна инструкция inline-ассемблера:

```c
static inline void __set_bit(long nr, volatile unsigned long *addr)
{
    asm volatile("bts %1,%0" : ADDR : "Ir" (nr) : "memory");
}
```

- `bts` выбирает бит `nr`, сохраняет старое значение в флаг `CF`
  и ставит новый; `btr` — снимает, `btc` — инвертирует, `bt` —
  только проверяет;
- `ADDR` = `"+m" (*(volatile long *)(addr))` — операнд памяти
  на вход и выход; ограничения `I` — целая константа, `r` — регистр;
- `"memory"` — компилятору сообщается об изменении памяти.

Атомарная версия `set_bit`:

```c
if (IS_IMMEDIATE(nr)) {          /* номер известен на этапе сборки */
    asm volatile(LOCK_PREFIX "orb %1,%0" ...);   /* or байта */
} else {
    asm volatile(LOCK_PREFIX "bts %1,%0" ...);   /* lock bts */
}
```

`IS_IMMEDIATE` = `__builtin_constant_p(nr)`: известный номер бита
обрабатывается быстрым `or` по маске `CONST_MASK(nr) = 1 << (nr & 7)`
(адрес смещается на `nr >> 3` байта). `LOCK_PREFIX` раскрывается в
инструкцию `lock`, гарантирующую атомарность. `__always_inline` —
функция всегда встраивается (уменьшение образа ядра).

`test_bit` диспетчеризуется на `constant_test_bit` (побитовое `and`
с маской) или `variable_test_bit` (инструкции `bt` + `sbb`, результат
через `CF` → `oldbit`).

### Общие операции bitmap

Итераторы (`include/linux/bitops.h`):

```c
#define for_each_set_bit(bit, addr, size) \
    for ((bit) = find_first_bit((addr), (size)); \
         (bit) < (size); \
         (bit) = find_next_bit((addr), (size), (bit) + 1))
```

Аналоги: `for_each_set_bit_from` (с заданной стартовой позиции),
`for_each_clear_bit`, `for_each_clear_bit_from` — по очищенным битам.

`include/linux/bitmap.h`:

| Функция | Действие |
| --- | --- |
| `bitmap_zero(dst, nbits)` | обнулить массив (`0UL` или `memset`) |
| `bitmap_fill(dst, nbits)` | заполнить единицами (`0xff` + хвостовая маска) |
| `bitmap_copy` | копия (`memcpy` вместо `memset`) |
| `bitmap_and` / `bitmap_or` / `bitmap_xor` | побитовые операции |

`small_const_nbits(nbits)` — бит известен на сборке и не превышает
`BITS_PER_LONG` (64): тогда достаточно присваивания одному `long`.

---

## Шпаргалка команд и функций

### Команды

| Команда | Что показывает |
| --- | --- |
| `cat /proc/zoneinfo` | зоны памяти узлов NUMA |
| `cat /proc/iomem` | карту физической памяти и владельцев |
| `cat /proc/softirqs` | счётчики softirq по CPU (в т.ч. `RCU`) |
| `cat /proc/self/limits` | лимиты ресурсов (`RLIMIT_NPROC` и др.) |
| `dmesg \| grep hash` | размер хеш-таблицы PID |
| `dmesg \| grep SMBIOS` | найденную таблицу DMI |
| `ps -ef \| grep kthreadd` | поток `kthreadd` (PID 2) |
| `memblock=debug` в cmdline | отладку раннего аллокатора |
| `io_delay=0xed` в cmdline | смену порта задержки I/O |

### Порядок ключевых вызовов

```text
startup_64 -> __startup_64 -> x86_64_start_kernel
  -> start_kernel
      -> setup_arch (trap, e820, DMI, SMP, memblock, vsyscall)
      -> build_all_zonelists -> page_alloc_init -> pidhash_init
      -> mm_init -> sched_init -> preempt_disable
      -> idr_init_cache -> rcu_init -> trace_init -> radix_tree_init
      -> IRQ/таймеры -> local_irq_enable -> console_init
      -> cred_init -> fork_init -> proc_caches_init
      -> proc_root_init -> rest_init
          -> kernel_thread(kernel_init, PID 1)
          -> kernel_thread(kthreadd, PID 2)
          -> idle-цикл (PID 0)
kernel_init -> do_basic_setup -> run_init_process -> init
```

---

> Материалы глав `linux-initialization-1`–`10` и
> `linux-datastructures-1`–`3` книги
> [linux-insides](https://github.com/0xax/linux-insides).

**Обновлено:** 23.09.2026
**Автор:** Ivanov_M_V
