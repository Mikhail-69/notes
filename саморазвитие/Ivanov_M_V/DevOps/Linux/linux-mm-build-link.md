# Память Linux, сборка ядра и запуск программ

> Конспект глав книги `linux-insides`: `linux-mm-1`–`linux-mm-3`,
> `linux-misc-1`–`linux-misc-4` (исходники на английском — переведены).
> Memblock, fixmap, ioremap, kmemcheck, разработка и сборка ядра,
> линковка объектных файлов и старт программы в userspace.
> Про прерывания — в конспекте [здесь](linux-interrupts.md).

---

## 📌 Оглавление

- [Memblock: раннее управление памятью](#memblock-раннее-управление-памятью)
  - [Структуры данных](#структуры-данных)
  - [Инициализация memblock](#инициализация-memblock)
  - [Добавление регионов](#добавление-регионов)
  - [Слияние регионов и memblock_reserve](#слияние-регионов-и-memblock_reserve)
  - [Остальное API и отладка](#остальное-api-и-отладка)
- [Fix-mapped адреса](#fix-mapped-адреса)
  - [fix_to_virt и virt_to_fix](#fix_to_virt-и-virt_to_fix)
- [ioremap](#ioremap)
  - [I/O порты и дерево ресурсы](#io-порты-и-дерево-ресурсы)
  - [I/O память и /proc/iomem](#io-память-и-prociomem)
  - [Ранняя инициализация ioremap](#ранняя-инициализация-ioremap)
  - [early_ioremap и early_iounmap](#early_ioremap-и-early_iounmap)
- [kmemcheck: отладка памяти в ядре](#kmemcheck-отладка-памяти-в-ядре)
  - [Идея механизма](#идея-механизма)
  - [Два этапа инициализации](#два-этапа-инициализации)
  - [Сокрытие страниц](#сокрытие-страниц)
  - [Обработка page fault](#обработка-page-fault)
- [Разработка ядра: от исходников к патчу](#разработка-ядра-от-исходников-к-патчу)
  - [Получение исходников](#получение-исходников)
  - [Конфигурация и сборка](#конфигурация-и-сборка)
  - [Установка или запуск в qemu](#установка-или-запуск-в-qemu)
  - [Работа над патчем](#работа-над-патчем)
  - [Отправка патча](#отправка-патча)
  - [Советы разработчику](#советы-разработчику)
- [Процесс сборки ядра](#процесс-сборки-ядра)
  - [Переменные версии и опции make](#переменные-версии-и-опции-make)
  - [Архитектура и компиляторы в Makefile](#архитектура-и-компиляторы-в-makefile)
  - [Подготовка: prepare и scripts](#подготовка-prepare-и-scripts)
  - [Сборка vmlinux](#сборка-vmlinux)
  - [Сборка bzImage](#сборка-bzimage)
- [Линковка объектных файлов](#линковка-объектных-файлов)
  - [Объектные файлы и символы](#объектные-файлы-и-символы)
  - [Релокация](#релокация)
  - [GNU ld на практике](#gnu-ld-на-практике)
  - [Полезные опции ld](#полезные-опции-ld)
  - [Linker scripts](#linker-scripts)
  - [Функции и символы скрипта](#функции-и-символы-скрипта)
- [Запуск программы в userspace](#запуск-программы-в-userspace)
  - [Точка входа не main](#точка-входа-не-main)
  - [Как ядро запускает программу](#как-ядро-запускает-программу)
  - [Откуда берётся _start](#откуда-берётся-_start)
  - [Файлы crt и секции init](#файлы-crt-и-секции-init)
  - [Цепочка вызовов до main](#цепочка-вызовов-до-main)
- [Шпаргалка](#шпаргалка)

---

## Memblock: раннее управление памятью

**Memblock** — механизм управления участками памяти на раннем этапе
загрузки, пока обычные распределители ядра ещё не работают. Раньше он
назывался `Logical Memory Block`, патчем Янхай Лу переименован в
`memblock`. Именно им пользуется ядро для `x86_64`.

### Структуры данных

Всё определено в `include/linux/memblock.h`. Главная структура:

```c
struct memblock {
         bool bottom_up;
         phys_addr_t current_limit;
         struct memblock_type memory;   /* массив memblock_region */
         struct memblock_type reserved; /* массив memblock_region */
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
         struct memblock_type physmem;
#endif
};
```

Поля:

| Поле | Назначение |
| --- | --- |
| `bottom_up` | распределение снизу вверх, если `true` |
| `current_limit` | предел размера блока памяти |
| `memory` | обычные регионы памяти |
| `reserved` | зарезервированные регионы |
| `physmem` | физическая память (при `CONFIG_HAVE_MEMBLOCK_PHYS_MAP`) |

Тип коллекции регионов:

```c
struct memblock_type {
    unsigned long cnt;
    unsigned long max;
    phys_addr_t total_size;
    struct memblock_region *regions;
};
```

Здесь `cnt` — число регионов, `max` — ёмкость массива, `total_size` —
суммарный размер, `regions` — указатель на массив `memblock_region`.
Сам регион:

```c
struct memblock_region {
        phys_addr_t base;
        phys_addr_t size;
        unsigned long flags;
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
        int nid;
#endif
};
```

Допустимые флаги `flags`:

```c
enum {
    MEMBLOCK_NONE    = 0x0,    /* особых требований нет */
    MEMBLOCK_HOTPLUG    = 0x1,    /* регион можно добавлять на лету */
    MEMBLOCK_MIRROR    = 0x2,    /* зеркальный регион */
    MEMBLOCK_NOMAP    = 0x4,    /* не добавлять в прямое отображение */
};
```

Поле `nid` — номер узла NUMA (при `CONFIG_HAVE_MEMBLOCK_NODE_MAP`).
Схематично: структура `memblock` содержит два (или три) поля типа
`memblock_type`, каждое из которых указывает на свой массив
`memblock_region`. Эти три структуры — основа всего memblock.

### Инициализация memblock

Описания функций — в `include/linux/memblock.h`, реализация — в
`mm/memblock.c`. В начале файла определён сам объект:

```c
struct memblock memblock __initdata_memblock = {
    .memory.regions        = memblock_memory_init_regions,
    .memory.cnt            = 1,
    .memory.max            = INIT_MEMBLOCK_REGIONS,

    .reserved.regions    = memblock_reserved_init_regions,
    .reserved.cnt        = 1,
    .reserved.max        = INIT_MEMBLOCK_REGIONS,
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
    .physmem.regions    = memblock_physmem_init_regions,
    .physmem.cnt        = 1,
    .physmem.max        = INIT_PHYSMEM_REGIONS,
#endif
    .bottom_up            = false,
    .current_limit        = MEMBLOCK_ALLOC_ANYWHERE,
};
```

Атрибут `__initdata_memblock` зависит от `CONFIG_ARCH_DISCARD_MEMBLOCK`:
если опция включена, код и данные memblock попадают в секцию `.init` и
**освобождаются после загрузки ядра**. Массивы регионов инициализируются
так:

```c
static struct memblock_region memblock_memory_init_regions
    [INIT_MEMBLOCK_REGIONS] __initdata_memblock;
```

Каждый массив вмещает `INIT_MEMBLOCK_REGIONS` = **128** регионов.
`bottom_up` выключен, а лимит `MEMBLOCK_ALLOC_ANYWHERE` определён как
`~(phys_addr_t)0`, то есть `0xffffffffffffffff` — распределять можно
где угодно.

### Добавление регионов

Пример использования — функция `memblock_x86_fill` из
`arch/x86/kernel/e820.c`: она проходит по карте памяти **e820** и
добавляет регионы вызовом `memblock_add`. Сама функция `memblock_add`
обёртка над:

```c
memblock_add_range(&memblock.memory, base, size, MAX_NUMNODES, 0);
```

`MAX_NUMNODES` равно 1, если `CONFIG_NODES_SHIFT` не задан, иначе
`1 << CONFIG_NODES_SHIFT`. Алгоритм `memblock_add_range`:

1. Если `size` равен нулю — выйти.
2. Если регионов ещё нет — заполнить первый `memblock_region` и закончить.
3. Вычислить конец региона: `end = base + memblock_cap_size(base, &size)`.
   Функция `memblock_cap_size` не даёт переполнению: размер усекается до
   `ULLONG_MAX - base`.
4. Пройти по уже хранящимся регионам и проверить пересечения.
5. Непересекающиеся части вставить как отдельные регионы.
6. Соседние совместимые регионы слить.

Если регионов слишком много для массива, вызывается удвоение ёмкости:

```c
while (type->cnt + nr_new > type->max)
    if (memblock_double_array(type, obase, size) < 0)
        return -ENOMEM;
```

Вставка нового региона в середину делается через `memmove` хвоста
массива и заполнения полей:

```c
memmove(rgn + 1, rgn, (type->cnt - idx) * sizeof(*rgn));
```

При пересечении с уже существующим регионом новый участок начинается с
конца накрытого региона (`base = min(rend, end)`), вставляется остаток,
затем всё сливается.

### Слияние регионов и memblock_reserve

Функция `memblock_merge_regions` объединяет соседние регионы, если
выполнены все условия:

- первый регион непосредственно касается второго
  (`this->base + this->size == next->base`);
- оба принадлежат одному узлу NUMA;
- флаги совпадают.

Тогда размер первого увеличивается, а второй удаляется:

```c
this->size += next->size;
memmove(next, next + 1, (type->cnt - (i + 2)) * sizeof(*next));
type->cnt--;
```

Функция `memblock_reserve` делает то же, что и `memblock_add`, но
записывает регион не в `memory`, а в `reserved`.

### Остальное API и отладка

Помимо добавления, memblock умеет:

- `memblock_remove` — удалить регион;
- `memblock_find_in_range` — найти свободный участок в диапазоне;
- `memblock_free` — освободить регион;
- `for_each_mem_range` — итерировать регионы.

Получение информации о коллекциях — функции
`get_allocated_memblock_memory_regions_info` и
`get_allocated_memblock_reserved_regions_info`. Они возвращают ноль,
если коллекция не выделялась, иначе — физический адрес массива и размер
с выравниванием `PAGE_ALIGN` (округление до `PAGE_SIZE`).

Для отладки передайте ядру параметр `memblock=debug`: функция
`memblock_dbg` (обёртка над `printk`) начнёт печатать, например:

```text
memblock_reserve: [0x0000000000001000-0x0000000000001fff] ...
```

Также memblock представлен в debugfs (на не-x86 архитектурах):

- `/sys/kernel/debug/memblock/memory`
- `/sys/kernel/debug/memblock/reserved`
- `/sys/kernel/debug/memblock/physmem`

---

## Fix-mapped адреса

**Fix-mapped адреса** — набор специальных адресов, задаваемых на этапе
компиляции; их физический адрес вычисляется не как «линейный минус
`__START_KERNEL_map`». Каждый такой адрес отображает один кадр памяти и
**никогда не меняется**. Комментарий в исходниках: «иметь постоянный
адрес при компиляции, но назначать физический адрес только при
загрузке».

Ранее, при сборке таблиц страниц, была подготовлена таблица
`level2_fixmap_pgt`:

```asm
NEXT_PAGE(level2_fixmap_pgt)
    .fill    506,8,0
    .quad    level1_fixmap_pgt - __START_KERNEL_map + _PAGE_TABLE
    .fill    5,8,0

NEXT_PAGE(level1_fixmap_pgt)
    .fill    512,8,0
```

Она находится сразу после `level2_kernel_pgt` (код + данные + bss
ядра). Каждый fix-mapped адрес представлен целочисленным индексом из
перечисления `fixed_addresses` (`arch/x86/include/asm/fixmap.h`),
например `VSYSCALL_PAGE` (эмуляция vsyscall-страницы) или
`FIX_APIC_BASE` (локальный APIC). В виртуальном адресном пространстве
область лежит в зоне модулей:

```text
       +-----------+-----------------+---------------+------------------+
       |kernel text|      kernel     |               |    vsyscalls     |
       | mapping   |       text      |    Modules    |    fix-mapped    |
       |from phys 0|       data      |               |    addresses     |
       +-----------+-----------------+---------------+------------------+
__START_KERNEL_map  __START_KERNEL   MODULES_VADDR    0xffff...ffff
```

Базовый адрес и размер области:

```c
#define FIXADDR_SIZE    (__end_of_permanent_fixed_addresses << PAGE_SHIFT)
#define FIXADDR_START    (FIXADDR_TOP - FIXADDR_SIZE)
```

`__end_of_permanent_fixed_addresses` — последний индекс `fixed_addresses`,
то есть число страниц области (у автора чуть более 536 КБ; зависит от
конфигурации). `FIXADDR_TOP` — адрес, округлённый вверх от базы
vsyscall-пространства:

```c
#define FIXADDR_TOP \
    (round_up(VSYSCALL_ADDR + PAGE_SIZE, 1<<PMD_SHIFT) - PAGE_SIZE)
```

### fix_to_virt и virt_to_fix

Прямое преобразование «индекс → адрес»:

```c
static __always_inline unsigned long fix_to_virt(const unsigned int idx)
{
        BUILD_BUG_ON(idx >= __end_of_fixed_addresses);
        return __fix_to_virt(idx);
}

#define __fix_to_virt(x)  (FIXADDR_TOP - ((x) << PAGE_SHIFT))
```

Индекс сдвигается на размер страницы и вычитается из вершины области.
Обратное преобразование «адрес → индекс»:

```c
static inline unsigned long virt_to_fix(const unsigned long vaddr)
{
        BUG_ON(vaddr >= FIXADDR_TOP || vaddr < FIXADDR_START);
        return __virt_to_fix(vaddr);
}

#define __virt_to_fix(x)  ((FIXADDR_TOP - ((x)&PAGE_MASK)) >> PAGE_SHIFT)
```

Адрес очищается до базы страницы (`x & PAGE_MASK` обнуляет младшие 12
бит), вычитается из `FIXADDR_TOP`, результат делится на размер страницы
(`>> PAGE_SHIFT`) — получаем индекс.

Где используются fix-mapped адреса: дескриптор **IDT**, UUID технологии
Intel Trusted Execution (индекс `FIX_TBOOT_BASE`), bootmap Xen и, что
важно для следующего раздела, **ранний ioremap**.

---

## ioremap

Устройства управляются чтением и записью их регистров. Есть два способа
доступа:

- через **I/O порты** (инструкции `in` и `out`);
- отображение регистров в **адресное пространство памяти**
  (memory-mapped I/O).

### I/O порты и дерево ресурсы

Зарегистрированные диапазоны портов видны в `/proc/ioports`:

```text
0000-0cf7 : PCI Bus 0000:00
  0020-0021 : pic1
  0040-0043 : timer0
  0070-0077 : rtc0
  03f8-03ff : serial
```

Каждый диапазон запрашивается макросом `request_region`:

```c
#define request_region(start,n,name) \
    __request_region(&ioport_resource, (start), (n), (name), 0)
```

Параметры: начало, длина и имя запросившего. Перед этим часто вызывают
`check_region`, после — `release_region`. Возвращается указатель на
структуру `resource` — узел дерева ресурсов:

```c
struct resource {
        resource_size_t start;
        resource_size_t end;
        const char *name;
        unsigned long flags;
        struct resource *parent, *sibling, *child;
};
```

У каждого дерева есть корень. Для портов это `ioport_resource`
(«PCI IO», `IORESOURCE_IO`), для памяти — `iomem_resource`
(«PCI mem», `IORESOURCE_MEM`).

Пример из `drivers/char/rtc.c`: модуль RTC в инициализаторе
`rtc_init` вызывает `request_region(RTC_PORT(0), size, "rtc")`, где
`RTC_PORT(x)` = `0x70 + (x)`, размер `0x8`. В результате в `/proc/ioports`
появляется строка `0070-0077 : rtc0`.

### I/O память и /proc/iomem

Для memory-mapped устройств физический адрес моста нужно перевести в
виртуальный адрес ядра — этим занимается **`ioremap`** (параметры:
начало региона и размер). Регистрация диапазонов памяти — функции
`request_mem_region`, `release_mem_region`, `check_mem_region`.
Карта видна в `/proc/iomem`:

```text
be82d000-bf744fff : System RAM
bf745000-bfff4fff : reserved
f7c10000-f7c101ff : ahci
```

Часть строк создаёт функция `e820_reserve_resources`
(`arch/x86/kernel/e820.c`): она вставляет регионы карты e820 в корень
`iomem_resource`. Типы регионов и подписи в `/proc/iomem`:

```c
E820_RAM       -> "System RAM"
E820_ACPI      -> "ACPI Tables"
E820_NVS       -> "ACPI Non-volatile Storage"
E820_UNUSABLE  -> "Unusable memory"
иначе          -> "reserved"
```

### Ранняя инициализация ioremap

Инициализация разделяется на раннюю (до vmalloc и `paging_init`) и
обычную. Ранняя начинается с `early_ioremap_init`
(`arch/x86/mm/ioremap.c`), которая проверяет выравнивание fixmap по
границе средней директории:

```c
BUILD_BUG_ON((fix_to_virt(0) + PAGE_SIZE) & ((1 << PMD_SHIFT) - 1));
```

Затем вызывается `early_ioremap_setup` (`mm/early_ioremap.c`), она
заполняет массив `slot_virt` виртуальными адресами ранних fixmap.
Всего доступно **512 временных отображений**:

```c
#define NR_FIX_BTMAPS        64
#define FIX_BTMAPS_SLOTS    8
#define TOTAL_FIX_BTMAPS    (NR_FIX_BTMAPS * FIX_BTMAPS_SLOTS)
```

Ранние fixmap идут после `__end_of_permanent_fixed_addresses`: от
`FIX_BTMAP_BEGIN` (сверху) вниз до `FIX_BITMAP_END`. Рядом определены
массивы `prev_map` (занятые адреса), `prev_size` и `slot_virt` — все
с атрибутом `__initdata`, то есть освобождаются после загрузки.

Дальше находится PMD, в который начнётся ранний ioremap:

```c
static inline pmd_t * __init early_ioremap_pmd(unsigned long addr)
{
    pgd_t *base = __va(read_cr3());
    pgd_t *pgd = &base[pgd_index(addr)];
    pud_t *pud = pud_offset(pgd, addr);
    pmd_t *pmd = pmd_offset(pud, addr);
    return pmd;
}
```

Таблица записей `bm_pte` обнуляется и привязывается к PMD:

```c
pmd = early_ioremap_pmd(fix_to_virt(FIX_BTMAP_BEGIN));
memset(bm_pte, 0, sizeof(bm_pte));
pmd_populate_kernel(&init_mm, pmd, bm_pte);
```

`pmd_populate_kernel` записывает физический адрес `bm_pte` в запись
средней директории с флагом `_PAGE_TABLE` (через `set_pmd` →
`native_set_pmd`, простое присваивание). После этого ранний ioremap
готов.

### early_ioremap и early_iounmap

API раннего этапа — две функции (зависят от `CONFIG_MMU`; без MMU
`early_ioremap` возвращает физический адрес, а `early_iounmap` ничего
не делает):

- `early_ioremap(phys_addr, size, prot)` — отобразить;
- `early_iounmap(phys_addr, size)` — снять отображение.

Алгоритм `__early_ioremap`:

1. Найти свободный слот в массиве `prev_map`, запомнить `prev_size[slot]`.
2. Выровнять адрес и размер по границе страницы через `PAGE_MASK`:
   `phys_addr &= PAGE_MASK`, размер `PAGE_ALIGN(last_addr + 1)`.
3. Вычислить число страниц `nrpages = size >> PAGE_SHIFT` и индекс
   `idx = FIX_BTMAP_BEGIN - NR_FIX_BTMAPS * slot`.
4. В цикле для каждой страницы вызвать `__early_set_fixmap`,
   увеличив физический адрес на `PAGE_SIZE`.
5. `__early_set_fixmap` находит PTE (`early_ioremap_pte`) и вызывает
   `set_pte` (если флаги не нулевые) иначе `pte_clear`. Передаются
   флаги `FIXMAP_PAGE_IO` = `__PAGE_KERNEL_EXEC | _PAGE_NX`.
6. Инвалидировать запись TLB: `__flush_tlb_one(addr)`. Если CPU
   поддерживает `invlpg` (`cpu_has_invlpg`), выполняется
   `invlpg (%0)`; иначе перезаписывается регистр `cr3`
   (`native_write_cr3(native_read_cr3())`).
7. Сохранить результат: `prev_map[slot] = offset + slot_virt[slot]`.

`early_iounmap` находит слот по адресу и очищает его: до
`paging_init` — через `__early_set_fixmap` с нулевым физическим
адресом, после — через `__late_clear_fixmap`; затем
`prev_map[slot] = NULL`.

Обычный (не ранний) `ioremap` работает после инициализации vmalloc —
он разбирается отдельно и требует знания распределителей памяти.

---

## kmemcheck: отладка памяти в ядре

### Идея механизма

**kmemcheck** — инструмент отладки, аналог **valgrind** для ядра: он
проверяет обращения к **неинициализированной памяти**. Пример из
пользовательского пространства: программа выделяет структуру через
`malloc` и печатает незаполненное поле — компильтор молчит, а valgrind
ловит:

```text
Conditional jump or move depends on uninitialised value(s)
   at ...: vfprintf ...
```

Включается опцией конфигурации `CONFIG_KMEMCHECK`
(меню `Kernel hacking → Memory Debugging`). Реализация есть **только
для x86_64** (в `arch/x86/Kconfig`: `select HAVE_ARCH_KMEMCHECK`).

Принцип работы: когда ядро выделяет память

```c
struct my_struct *my_struct = kmalloc(sizeof(struct my_struct),
                                      GFP_KERNEL);
```

страницы помечаются как **non-present**. При первом обращении к ним
происходит page fault, обработчик передаёт управление kmemcheck; после
проверки страница снова помечается present — но **после выполнения
первой же инструкции** над ней скрывается вновь, чтобы поймать следующее
обращение. Так проверка продолжается постоянно.

Два режима работы, задаются параметром командной строки `kmemcheck=`:

| Значение | Режим |
| --- | --- |
| `0` | выключен |
| `1` | включён |
| `2` | one-shot: выключится после первой найденной ошибки |

Режим one-shot по умолчанию.

### Два этапа инициализации

Первый этап — разбор командной строки (в `do_early_param` при
инициализации ядра). В `mm/kmemcheck.c` определено:

```c
static int __init param_kmemcheck(char *str)
{
    int val;
    int ret;

    if (!str)
        return -EINVAL;
    ret = kstrtoint(str, 0, &val);
    if (ret)
        return ret;
    kmemcheck_enabled = val;
    return 0;
}

early_param("kmemcheck", param_kmemcheck);
```

Значение строки переводится в число и записывается в
`kmemcheck_enabled`.

Второй этап — ранний initcall:

```c
int __init kmemcheck_init(void)
{
    /* ... */
}

early_initcall(kmemcheck_init);
```

Он запускает `kmemcheck_selftest`, который сверяет размеры опкодов
работы с памятью (`rep movsb`, `movzwq` и т. п.) с ожидаемыми. При
провале выводится `kmemcheck: self-tests failed; disabling`,
`kmemcheck_enabled` обнуляется, функция возвращает `-EINVAL`.

### Сокрытие страниц

Цепочка выделения: `kmalloc` → … → `kmem_getpages` (`mm/slab.c`):

```c
if (kmemcheck_enabled && !(cachep->flags & SLAB_NOTRACK)) {
    kmemcheck_alloc_shadow(page, cachep->gfporder, flags, nodeid);

    if (cachep->ctor)
        kmemcheck_mark_uninitialized_pages(page, nr_pages);
    else
        kmemcheck_mark_unallocated_pages(page, nr_pages);
}
```

`SLAB_NOTRACK` запрещает отслеживание для данного кеша. Если у объекта
есть конструктор — страницы помечаются как неинициализированные, иначе
— как невыделенные.

`kmemcheck_alloc_shadow` выделяет «теневые» биты (флаг `__GFP_NOTRACK`),
привязывает их к страницам через `page[i].shadow` и вызывает
`kmemcheck_hide_pages` — архитектурную функцию из
`arch/x86/mm/kmemcheck/kmemcheck.c`:

```c
void kmemcheck_hide_pages(struct page *p, unsigned int n)
{
    unsigned int i;

    for (i = 0; i < n; ++i) {
        unsigned long address;
        pte_t *pte;
        unsigned int level;

        address = (unsigned long) page_address(&p[i]);
        pte = lookup_address(address, &level);
        BUG_ON(!pte);
        BUG_ON(level != PG_LEVEL_4K);

        set_pte(pte, __pte(pte_val(*pte) & ~_PAGE_PRESENT));
        set_pte(pte, __pte(pte_val(*pte) | _PAGE_HIDDEN));
        __flush_tlb_one(address);
    }
}
```

Для каждой страницы снимается бит present, ставится бит hidden,
обновляется TLB. С этого момента страницами «владеет» kmemcheck, и
любой доступ спровоцирует page fault.

### Обработка page fault

Обработчик исключения — `do_page_fault` / `__do_page_fault`
(`arch/x86/mm/fault.c`). В начале:

```c
if (kmemcheck_active(regs))
    kmemcheck_hide(regs);
```

`kmemcheck_active` смотрит на per-cpu структуру `kmemcheck_context`:
если её поле `balance` больше нуля — страницы уже показаны, и их нужно
снова скрыть (одна проверка кончилась, началась следующая).

Дальше:

```c
if (kmemcheck_fault(regs, address, error_code))
        return;
```

`kmemcheck_fault` проверяет, что fault действительно наш:

- не виртуальный режим (`regs->flags & X86_VM_MASK` → отказ);
- кодовый сегмент ядра (`regs->cs != __KERNEL_CS` → отказ);
- есть PTE для адреса (`kmemcheck_pte_lookup`).

Затем вызывается `kmemcheck_access` — основная работа: проверка
вызвавшей fault инструкки. Ошибки складываются в кольцевой буфер:

```c
static struct kmemcheck_error
    error_fifo[CONFIG_KMEMCHECK_QUEUE_SIZE];
```

Для вывода объявлен tasklet:

```c
static DECLARE_TASKLET(kmemcheck_tasklet, &do_wakeup, 0);
```

`do_wakeup` (`arch/x86/mm/kmemcheck/error.c`) вызывает
`kmemcheck_error_recall` и печатает накопленные ошибки.

В конце `kmemcheck_fault` вызывает `kmemcheck_show`, который ставит
бит present обратно (`kmemcheck_show_all` → `kmemcheck_show_addr` для
каждого адреса, там `set_pte(... | _PAGE_PRESENT)` + сброс TLB) и
включает флаг **TF** (Trap Flag) регистров, если его не было:

```c
if (!(regs->flags & X86_EFLAGS_TF))
    data->flags = regs->flags;
```

С TF процессор после выполнения первой инструкции войдёт в режим
одного шага и сгенерирует #DB — в этот момент страницы скрываются
снова. Цикл «fault → проверка → показ → одна инструкция → скрытие»
повторяется.

---

## Разработка ядра: от исходников к патчу

### Получение исходников

Запустить своё ядро можно на виртуальной машине или на реальном
железе. Обновить установленное ядро можно пакетным менеджером
дистрибутива, но для разработки нужны исходники:

```bash
git clone git://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
```

Есть зеркало на GitHub (`github.com/torvalds/linux`). Типовой сетап —
свой fork и remote основного репозитория:

```bash
git remote add upstream git@github.com:torvalds/linux.git
git checkout master
git pull upstream master
```

`origin` — ваш fork, `upstream` — основной репозиторий.

### Конфигурация и сборка

Конфигурацию можно скопировать с текущего ядра:

```bash
sudo cp /boot/config-$(uname -r) ~/dev/linux/.config
cat /proc/config.gz | gunzip > ~/dev/linux/.config
```

Или сгенерировать заново: `make menuconfig` (текстовое меню),
`make defconfig` (конфиг по умолчанию для архитектуры; например
`make ARCH=arm64 defconfig`), `allnoconfig` / `allyesconfig` /
`allmodconfig` (всё выкл / всё вкл / всё модулями), `nconfig`
(ncurses-меню), `randconfig` (случайный).

Сборка и ускорение:

```bash
make          # обычная сборка
make -j8      # параллельно 8 задач
```

Кросс-сборка для другой архитектуры:

```bash
make -j4 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
make -j4 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

Результат — сжатый образ `arch/x86/boot/bzImage`.

### Установка или запуск в qemu

На реальном железе:

```bash
sudo make headers_install
sudo make modules_install
sudo make install
```

Затем обновить конфиг загрузчика: в Fedora —
`grub2-mkconfig -o /boot/grub2/grub.cfg`, в Ubuntu — `update-grub`.

Для запуска в виртуальной машине сначала соберите `initrd` на базе
**busybox** (статическая сборка: `Build BusyBox as a static binary`):

```bash
mkdir initramfs && cd initramfs
mkdir -pv {bin,sbin,etc,proc,sys,usr/{bin,sbin}}
cp -av ../busybox/_install/* .
```

Создайте файл `init` — первый процесс системы:

```sh
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
exec /bin/sh
```

Соберите архив и запустите ядро в **qemu**:

```bash
find . -print0 | cpio --null -ov --format=newc | gzip -9 > initrd.gz

qemu-system-x86_64 -snapshot -m 8G -serial stdio \
  -kernel arch/x86/boot/bzImage -initrd initrd.gz \
  -append "root=/dev/sda1 ignore_loglevel"
```

Для автоматизации initrd годятся `ivandavidov/minimal` и Buildroot.

### Работа над патчем

Синхронизируйтесь с mainline, создайте ветку, внесите правку. Хорошее
место для старта — дерево **staging** (`drivers/staging`,
мейнтейнер Greg Kroah-Hartman). Пример: в драйвере `dgap` функция
`dgap_sindex` дублирует стандартную `strpbrk` из `lib/string.c` —
её удаляют и заменяют вызовом стандартной.

```bash
git checkout -b "dgap-remove-dgap_sindex"
git add .
git commit -s -v
```

`-s` добавляет строку `Signed-off-by` (кто автор изменения), `-v`
показывает diff в редакторе коммита. Формат сообщения:

```text
[PATCH] staging/dgap: Use strpbrk() instead of dgap_sindex()

The <linux/string.h> provides strpbrk() function that does the same
that the dgap_sindex(). Let's use already defined function instead
of writing custom.
```

Первая строка — `[PATCH]`, подсистема, `:` и краткое описание; далее
пустая строка и подробности. Каждая строка — **не длиннее 80
символов**; не пишите «Custom function removed» — объясняйте, что и
почему сделано: по сообщениям коммитов читают историю через
`git blame`.

### Отправка патча

```bash
git format-patch master
```

Создаст файл `.patch` по названию коммита (свой именительный файл —
через `--stdout > file.patch`). Найдите мейнтейнеров подсистемы:

```bash
./scripts/get_maintainer.pl -f drivers/staging/dgap/dgap.c
```

И отправьте письмом (обязательно plain text):

```bash
git send-email --to "maintainer <mail@example.com>" \
  --cc "subsystem <mail@example.com>" 0001-staging-dgap-....patch
```

В рассылку `linux-kernel@vger.kernel.org` слать бессмысленно — патч
потонет в потоке. Путь патча: репозиторий мейнтейнера → его pull
request Линусу → mainline. Серии патчей: `git format-patch
--cover-letter` добавит cover letter, а `git send-email --in-reply-to`
свяжет серию с ним:

```text
|--> cover letter
  |----> patch_1
  |----> patch_2
```

Если патч отклонили — исправьте и отправьте снова с префиксом
`[PATCH v2]` и changelog изменений.

### Советы разработчику

- Думайте, прежде чем отправлять патч.
- После **каждого** изменения пересобирайте ядро — неработающий
  компилирующийся код никто не принимает.
- Стиль кода ядра обязателен; проверка:
  `./scripts/checkpatch.pl -f file.c`.
- Линус не принимает pull request'ы с GitHub — только письма через
  `git send-email`.
- Не связанные изменения разделяйте на разные коммиты (и патчи).
- Не ждите мгновенного ответа: мейнтейнеры заняты.
- Полезные скрипты в `scripts/`: `checkpatch.pl`, `get_maintainer.pl`,
  `extract-vmlinux` (извлечение несжатого образа), `stackusage`.
- Подпишитесь на рассылки: `lkml` и рассылки конкретных подсистем.

---

## Процесс сборки ядра

Что происходит, когда вы выполняете `make` в корне исходников ядра.
Топовый `Makefile` (1591 строка для ядра 4.2.0-rc3) отвечает за два
продукта: **vmlinux** (резидентный образ) и модули. В каждой директории
с исходами лежит свой Makefile/Kbuild; здесь разбирается только
стандартный сценарий — от `make` до `bzImage`.

### Переменные версии и опции make

Начало Makefile:

```make
VERSION = 4
PATCHLEVEL = 2
SUBLEVEL = 0
EXTRAVERSION = -rc3
NAME = Hurr durr I'ma sheep
```

Из них собирается `KERNELVERSION`. Дальше — обработка опций командной
строки:

- `V=1` (подробный вывод): `KBUILD_VERBOSE = $(V)`, затем `quiet` и
  `Q = @` (`@` подавляет эхо команды; без него печатается
  `Compiling ...`, с ним — короткое `CC ...`).
- `O=/dir` (вывод в каталог): `KBUILD_OUTPUT` создаётся через `mkdir -p`,
  затем make перезапускается рекурсивно с `-C` в этот каталог
  (`sub-make`).
- `C=1` — проверять исходники инструментом `$CHECK` (по умолчанию
  **sparse**).
- `M=dir` — собрать внешние модули (`KBUILD_EXTMOD`).

Каталоги исходников: если `KBUILD_SRC` пуст, `srctree := .` — дерево в
текущей директории; `objtree`, `src`, `obj` выставляются соответственно
и экспортируются.

### Архитектура и компиляторы в Makefile

Базовая архитектура вычисляется из `uname -m` через `sed`:

```make
SUBARCH := $(shell uname -m | sed -e s/i.86/x86/ -e s/x86_64/x86/ \
              -e s/arm.*/arm/ -e s/aarch64.*/arm64/ ... )
```

`ARCH` — псевдоним `SUBARCH`; для `i386`/`x86_64` каталог исходников —
`SRCARCH := x86`. Путь конфигурации — `KCONFIG_CONFIG ?= .config`,
оболочка — `CONFIG_SHELL` (bash, если доступен).

Компиляторы: `HOSTCC = gcc` / `HOSTCXX = g++` собирают **host-программы**
(инструменты, запускаемые во время сборки), а `CC` — целевой компилятор
ядра. Переменные `KBUILD_MODULES` и `KBUILD_BUILTIN` определяют, что
собирать (модули, ядро, оба); при `make modules` значение
`KBUILD_BUILTIN` зависит от `CONFIG_MODVERSIONS`.

Дальше подключается инфраструктура сборки:

```make
include scripts/Kbuild.include
```

**Kbuild** (Kernel Build System) — надстройка над make со своей
нотацией (`obj-y`, `hostprogs-y` и т. д.). Затем определяются инструменты
и их префиксы для кросс-компиляции:

```make
AS        = $(CROSS_COMPILE)as
LD        = $(CROSS_COMPILE)ld
CC        = $(CROSS_COMPILE)gcc
AR        = $(CROSS_COMPILE)ar
NM        = $(CROSS_COMPILE)nm
STRIP        = $(CROSS_COMPILE)strip
OBJCOPY        = $(CROSS_COMPILE)objcopy
OBJDUMP        = $(CROSS_COMPILE)objdump
```

Каталоги заголовков: `USERINCLUDE` (uapi-заголовки для пользователей) и
`LINUXINCLUDE` (заголовки ядра). Базовые флаги:

```make
KBUILD_CFLAGS   := -Wall -Wundef -Wstrict-prototypes -Wno-trigraphs \
           -fno-strict-aliasing -fno-common \
           -Werror-implicit-function-declaration \
           -std=gnu89
```

Они дополняются в других Makefile (например в `arch/`). В конце
экспортируются `RCS_FIND_IGNORE` и `RCS_TAR_IGNORE` — каталоги
систем контроля версий, которые игнорируются.

### Подготовка: prepare и scripts

Цель по умолчанию:

```make
all: vmlinux
    include arch/$(SRCARCH)/Makefile
```

Подключается архитектурный Makefile (`arch/x86/Makefile`), а цель
`vmlinux` зависит от `scripts/link-vmlinux.sh` и `vmlinux-deps`:

```make
vmlinux-deps := $(KBUILD_LDS) $(KBUILD_VMLINUX_INIT) $(KBUILD_VMLINUX_MAIN)
```

Это линковочный скрипт и `built-in.o` каждой верхнеуровневой директории
(`init/built-in.o`, `kernel/built-in.o`, `mm/built-in.o`,
`drivers/built-in.o`, `net/built-in.o`, …). Формируются они так: Kbuild
собирает все `$(obj-y)` директории и сливает через `$(LD) -r` в один
`built-in.o`.

Цепочка зависимостей:

```make
$(vmlinux-dirs): prepare scripts
    $(Q)$(MAKE) $(build)=$@
```

`prepare` разворачивается в несколько этапов:

```make
prepare: prepare0
prepare0: archprepare FORCE
archprepare: archheaders archscripts prepare1 scripts_basic
```

- `archheaders` генерирует таблицу системных вызовов
  (`arch/x86/entry/syscalls`);
- `archscripts` собирает инструмент `relocs` (`arch/x86/tools`) — он
  готовит данные релокаций (`relocs_32.c`, `relocs_64.c`);
- `scripts_basic` собирает host-программы `fixdep` (оптимизация
  списков зависимостей gcc) и `bin2c`. Первая строка обычной сборки —
  `HOSTCC scripts/basic/fixdep`;
- далее проверяется `include/config/kernel.release`, создаётся
  `version.h`, генерируются generic-заголовки asm.

Ключевая переменная:

```make
build := -f $(srctree)/scripts/Makefile.build obj
```

`scripts/Makefile.build` ищет в указанном каталоге файл `Kbuild`,
подключает его и строит цели оттуда. Цель `scripts` собирает остальные
host-инструменты: `modpost`, `file2alias`, `mk_elfconfig`.

Список директорий ядра:

```make
vmlinux-dirs := $(patsubst %/,%,$(filter %/, $(init-y) $(init-m) \
             $(core-y) $(core-m) $(drivers-y) $(drivers-m) \
             $(net-y) $(net-m) $(libs-y) $(libs-m)))
```

Пример значений: `init-y := init/`, `drivers-y := drivers/ sound/
firmware/`, `net-y := net/`, `libs-y := lib/`.

### Сборка vmlinux

Рекурсивный обход `vmlinux-dirs` идёт с флагом `$(build)=$@` — для
каждой директории запускается `make -f scripts/Makefile.build obj=<dir>`.
Вывод знаком:

```text
  CC      init/main.o
  AS      arch/x86/entry/entry_64.o
  CC      arch/x86/entry/syscall_64.o
  ...
```

Готовые объекты сливаются: в каждой директории появляется
`built-in.o` (`find . -name built-in.o`). После чего выполняется цель
`vmlinux`:

```make
vmlinux: scripts/link-vmlinux.sh $(vmlinux-deps) FORCE
    +$(call if_changed,link-vmlinux)
```

Скрипт `scripts/link-vmlinux.sh` линкует все `built-in.o` в один
статически связанный исполняемый файл и создаёт **System.map**:

```text
  LINK    vmlinux
  LD      vmlinux.o
  KSYM    .tmp_kallsyms1.o
  LD      vmlinux
  SORTEX  vmlinux
  SYSMAP  System.map
```

### Сборка bzImage

**bzImage** — сжатый образ ядра. Цель по умолчанию в
`arch/x86/Makefile`:

```make
all: bzImage

bzImage: vmlinux
    $(Q)$(MAKE) $(build)=$(boot) $(KBUILD_IMAGE)
```

`boot := arch/x86/boot`. Что происходит дальше:

1. В `arch/x86/boot` собирается загрузочный код, собирается
   `setup.elf` по линковочному скрипту `setup.ld`, из него —
   `setup.bin` (`objcopy -O binary`).
2. Для `header.o` нужны два производных заголовка: `voffset.h`
   (адреса начала и конца ядра, извлечённые `nm` из `vmlinux`:
   `VO__text`, `VO__end`) и `zoffset.h`.
3. В `arch/x86/boot/compressed` собирается сжатое ядро: создаётся
   `vmlinux.bin` (образ без отладочной информации), он дополняется
   таблицей релокаций (`vmlinux.relocs`, благодаря чему сжатое ядро
   может быть загружено по любому адресу), сжимается в
   `vmlinux.bin.bz2`.
4. Программа `mkpiggy` генерирует `piggy.S` — ассемблерный файл
   со смещением сжатого ядра, он компилируется в `piggy.o`.
5. Программа `arch/x86/boot/tools/build.c` склеивает `setup.bin` и
   `vmlinux.bin` в итоговый **bzImage**.

Финальный вывод:

```text
Setup is 16268 bytes (padded to 16384 bytes).
System is 4704 kB
CRC 94a88f9a
Kernel: arch/x86/boot/bzImage is ready  (#5)
```

---

## Линковка объектных файлов

**Линковщик** (linker) — программа, которая берёт один или несколько
объектных файлов, созданных компилятором, и объединяет их в единый
исполняемый файл, библиотеку или другой объектный файл. Объектные
файлы (`*.o`) — блоки машинного кода и данных с «плейсхолдерами»
адресов: они ссылаются на данные и функции других объектных файлов и
библиотек. Задача линковщика — собрать код и данные всех объектников
в финальный файл.

### Объектные файлы и символы

Небольшой проект:

```c
/* main.c */
#include <stdio.h>
#include "lib.h"

int main(int argc, char **argv) {
    printf("factorial of 5 is: %d\n", factorial(5));
    return 0;
}
```

```c
/* lib.c */
int factorial(int base) {
    int res,i = 1;

    if (base == 0) {
        return 1;
    }

    while (i <= base) {
        res *= i;
        i++;
    }

    return res;
}
```

Компилируем только `main.c` (`gcc -c main.c`) и смотрим символы:

```text
$ nm -A main.o
main.o:                 U factorial
main.o:0000000000000000 T main
main.o:                 U printf
```

Статус символа: `U` — **undefined** (не определён в этом объектнике),
`T` — символ лежит в секции `.text`. `factorial` и `printf` пока
неизвестны — их разрешит линковщик. В дизассемблировании висят
заглушки:

```text
  14:    e8 00 00 00 00           callq  19 <main+0x19>
  15:    R_X86_64_PC32           factorial-0x4
  25:    e8 00 00 00 00           callq  2a <main+0x2a>
  26:   R_X86_64_PC32           printf-0x4
```

`objdump -r` выводит записи **релокации**: какие адреса должен
подставить линковщик.

### Релокация

**Relocation** — процесс связывания символических ссылок с
символическими определениями и назначения адресов. Разберём
`e8 00 00 00 00`: `e8` — опкод `call`, остальное — 4-байтовое
смещение. Почему 4 байта, если адрес в x86_64 занимает 8? Потому что
по умолчанию действует модель кода `-mcmodel=small`: программа и её
символы линкуются в нижних 2 ГБ адресного пространства, смещения
достаточно в 4 байта.

После полной сборки смотрим вызов factorial:

```text
0000000000400506 <main>:
    40051a:    e8 18 00 00 00   callq  400537 <factorial>
```

Адрес `main` — `0x400506`, а не нулевой, потому что исполнение
начинается не с `main`, а из секции `.init` (конструкторы glibc;
адрес виден и в `readelf -d` в поле `INIT`, например `0x4003a8`).
Проверим арифметику смещения:

```python
>>> hex(0x40051a + 0x18 + 0x5) == hex(0x400537)
True
```

Смещение отсчитывается от адреса **следующей** инструкции; сам `call`
длинной 5 байт, поэтому прибавляем и `0x18`, и `0x5`. Компилятор
обычно собирает объектник с адресами от нуля; когда таких объектников
несколько, они перекрываются — релокация и разрешает это, назначая
адреса каждой части программы и поправляя код и данные.

### GNU ld на практике

`gcc` не линкует напрямую — он вызывает `collect2`, обёртку над
**GNU ld**. Попробуем линковать вручную:

```bash
ld main.o lib.o -o factorial
```

Получим две ошибки:

```text
ld: warning: cannot find entry symbol _start; ...
main.o: undefined reference to 'printf'
```

- **`_start`**, а не `main` — настоящая точка входа; она определена в
  объектном файле `crt1.o` (исходник — `sysdeps/x86_64/start.S` в
  glibc, там вызывается `__libc_start_main`).
- `printf` не найден — нужна стандартная библиотека (`-lc`).

Подключаем crt-файлы: `crti.o` (прологи секций `.init`/`.fini`,
символы `_init`/`_fini`) и `crtn.o` (эпилоги этих секций). Ошибки
`__libc_csu_fini`, `__libc_csu_init`, `__libc_start_main` уходят после
линковки с glibc. Полная команда:

```bash
ld /usr/lib/.../crt1.o /usr/lib/.../crti.o /usr/lib/.../crtn.o \
  main.o lib.o \
  -dynamic-linker /lib64/ld-linux-x86-64.so.2 \
  -lc -o factorial
```

Параметр `-dynamic-linker` нужен не просто так: запуск без него даёт
загадочную ошибку

```text
bash: ./factorial: No such file or directory
```

`readelf -l factorial` показывает строку
`[Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]`:
секция `.interp` исполняемого файла хранит путь к **динамическому
линковщику** — программе, которая при запуске копирует нужные
библиотеки с диска в RAM и завершает линковку. Указав этот путь
явно, получаем работающий бинарник:

```text
$ ./factorial
factorial of 5 is: 120
```

### Полезные опции ld

- `@file` — читать опции из файла (`ld @linker.ld`);
- `-b` / `--format`, `--oformat` — формат входных/выходных файлов
  (ELF, COFF…);
- `--defsym=symbol=expression` — задать символ с абсолютным адресом.
  Пример из ядра (`arch/arm/boot/compressed/Makefile`):

  ```make
  LDFLAGS_vmlinux = --defsym _kernel_bss_size=$(KBSS_SZ)
  ```

  Символ используется при распаковке ядра: `ldr r5, =_kernel_bss_size`;
- `-shared` — создать разделяемую библиотеку;
- `-M` — напечатать карту линковки с символами и их адресами;
- `--help`, `--version` — стандартно.

У `ld` есть собственный язык описания линковки.

### Linker scripts

Команды языка Linker Command Language лежат в файле, который передаётся
через `-T`. Главная команда — **`SECTIONS`**: она задаёт «карту»
выходного файла. Переменная `.` — текущая позиция вывода (location
counter). Пример: программа «hello, world» на ассемблере

```asm
.data
        msg:    .ascii  "hello, world!\n"

.text
.global _start
_start:
        mov    $1,%rax
        mov    $1,%rdi
        mov    $msg,%rsi
        mov    $14,%rdx
        syscall
        mov    $60,%rax
        mov    $0,%rdi
        syscall
```

и скрипт к ней:

```text
OUTPUT(hello)
OUTPUT_FORMAT("elf64-x86-64")
INPUT(hello.o)

SECTIONS
{
    . = 0x200000;
    .text : {
          *(.text)
    }

    . = 0x400000;
    .data : {
          *(.data)
    }
}
```

`OUTPUT`, `OUTPUT_FORMAT`, `INPUT` задают имя, формат и входной файл.
Внутри `SECTIONS`: `. = 0x200000` перемещает позицию вывода, далее
выходная секция `.text`, где `*(.text)` — «все секции `.text` из всех
входных файлов» (эквивалент `hello.o(.text)`). Затем `. = 0x400000` и
секция `.data`. Сборка и проверка:

```bash
as -o hello.o hello.S && ld -T linker.script && ./hello
objdump -D hello   # .text начинается с 0x200000, .data — с 0x400000
```

### Функции и символы скрипта

Проверка условий — `ASSERT(exp, message)`: если выражение нулевое,
линковщик падает с ошибкой. В линковочном скрипте самого Linux есть
проверка смещения заголовка setup:

```text
. = ASSERT(hdr == 0x1f1, "The setup header has the wrong offset!");
```

`INCLUDE filename` подключает другой скрипт. Символам можно присваивать
значения операторами C: `=`, `+=`, `-=`, `*=`, `/=`, `<<=`, `>>=`,
`&=`, `|=`:

```text
START_ADDRESS = 0x200000;
DATA_OFFSET   = 0x200000;

SECTIONS
{
    . = START_ADDRESS;
    .text : { *(.text) }
    . = START_ADDRESS + DATA_OFFSET;
    .data : { *(.data) }
}
```

Встроенные функции языка:

| Функция | Что делает |
| --- | --- |
| `ABSOLUTE` | абсолютное значение выражения |
| `ADDR` | адрес секции |
| `ALIGN` | позиция `.` после выравнивания |
| `DEFINED` | `1`, если символ есть в глобальной таблице |
| `MAX`, `MIN` | максимум/минимум двух выражений |
| `NEXT` | ближайший свободный кратный адрес |
| `SIZEOF` | размер секции в байтах |

---

## Запуск программы в userspace

### Точка входа не main

В университетах учат, что C-программа начинается с `main`. Проверим в
gdb: собираем простейшую программу и смотрим секции (`info files`):

```text
Entry point: 0x400430
0x00000000004003c8 - 0x00000000004003e2 is .init
0x0000000000400430 - 0x00000000004005e2 is .text
```

Ставим точку останова на адрес точки входа:

```text
(gdb) break *0x400430
(gdb) run
Breakpoint 1, 0x0000000000400430 in _start ()
```

Это **`_start`**, а не `main`. Откуда он берётся и кто в конце концов
вызывает `main` — дальше.

### Как ядро запускает программу

Запуск программы — семейство функций `exec*`, которые являются
обёртками над системным вызовом **`execve`**:

```c
SYSCALL_DEFINE3(execve,
        const char __user *, filename,
        const char __user *const __user *, argv,
        const char __user *const __user *, envp)
{
    return do_execve(getname(filename), argv, envp);
}
```

`do_execve` (`fs/exec.c`) выполняет проверки (корректность имени,
лимиты процессов), парсит исполняемый файл формата **ELF**, создаёт
новый дескриптор памяти и заполняет его (стек, куча и т. д.). Когда
образ готов, архитектурная функция `start_thread`
(`arch/x86/kernel/process_64.c`) задаёт новые значения сегментных
регистров и адрес выполнения. После переключения контекста управление
возвращается в пользовательское пространство — и новый образ
начинает исполняться.

### Откуда берётся _start

Компиляция `gcc -Wall program.c -o sum` состоит из шагов `cc1`
(C → ассемблер), `as` (ассемблер → объектник) и линковки через
`collect2`. В её выводе — подлинный вызов линковщика, и из него видно:
программа линкуется не только с libc, но и с объектными файлами
**crt**:

```text
... collect2 ... -dynamic-linker /lib64/ld-linux-x86-64.so.2 \
  -o program .../crt1.o .../crti.o .../crtbegin.o \
  ... /tmp/ccXXXX.o -lc ... .../crtend.o .../crtn.o
```

Проверка `gcc -nostdlib` (без библиотек) даёт ошибку
`cannot find entry symbol _start` — то есть `_start` приходит из
стандартной среды выполнения. Внутри `crt1.o` (`objdump -d`) —
символ `_start`. Исходный ассемблер — `sysdeps/x86_64/start.S` glibc.

Что делает `_start`:

1. Обнуляет `ebp` (требование System V ABI): `xorl %ebp, %ebp`.
2. Переносит адрес функции завершения из `rdx` в `r9` — он станет
   шестым аргументом `__libc_start_main` (по спецификации ELF у
   разделяемых объектов есть функции инициализации и завершения).
3. Достаёт аргументы со стека. В самом начале стек таков:

   ```text
   +-----------------+
   |       NULL      |
   |      envp       |
   |       NULL      |
   |      argv       |
   |      argc       | <- rsp
   +-----------------+
   ```

   `pop %rsi` извлекает `argc` (теперь `rsp` указывает на `argv`),
   `mov %rsp, %rdx` кладёт `argv` в `rdx`.
4. Выравнивает стек по границе 16 байт (`and $~15, %rsp`), кладёт
   адрес стека, загружает адреса деструктора и конструктора в `r8` и
   `rcx`, адрес `main` — в `rdi`.
5. Вызывает **`__libc_start_main`**.

### Файлы crt и секции init

Прототип `__libc_start_main` (`csu/libc-start.c`):

```c
STATIC int LIBC_START_MAIN (int (*main) (int, char **, char **),
                            int argc,
                            char **argv,
                            __typeof (main) init,
                            void (*fini) (void),
                            void (*rtld_fini) (void),
                            void *stack_end)
```

Аргументы: адрес `main`, `argc`/`argv`, конструктор, деструктор,
функция завершения динамического раздела, указатель на стек.

Конструкторы и деструкторы размещаются линковщиком в секциях **`.init`**
и **`.fini`** (видны через `readelf`). Инициализируются глобальные
переменные (например `errno`), выделяется память под системные
рутинки — всё это до настоящего кода программы. Секции определены в
`crti.o` (пролог `_init`: выравнивание стека, вызов `PREINIT_FUNCTION`
— `__gmon_start__` для профилирования), а эпилог (`addq $8, %rsp; ret`)
лежит в `crtn.o`. Отсюда известный эффект: собрав `gcc -nostdlib
crt1.o crti.o -lc ...`, получаем `Segmentation fault` (нет `ret`),
а добавив `crtn.o` — работающую программу:

```text
$ gcc -nostdlib /lib64/crt1.o /lib64/crti.o /lib64/crtn.o \
    -lc -ggdb program.c -o program
$ ./program
x + y + z = 6
```

### Цепочка вызовов до main

Линковщик по умолчанию помещает `_start` в начало секции `.text`
(видно из `ld --verbose | grep ENTRY` → `ENTRY(_start)`). Полная цепочка:

```text
_start (crt1.o, start.S)
  → подготовка argc/argv/envp, выравнивание стека
  → __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack)
       → регистрация конструкторов/деструкторов
       → запуск потоков, security-действия (canary стека)
       → инициализационные рутины
       → result = main(argc, argv, __environ)
       → exit(result)
```

То есть `main` — это просто функция, которую libc вызывает после всей
предварительной настройки; программирование «на низком уровне» и есть
попытка понять, что происходит до и после этих вызовов.

---

## Шпаргалка

### Команды анализа бинарников

| Команда | Что показывает |
| --- | --- |
| `nm file.o` | таблица символов (`U`, `T` — статус) |
| `objdump -S file.o` | дизассембл + исходники |
| `objdump -r file.o` | записи релокации |
| `objdump -d crt1.o` | дизассембл секции `.text` |
| `readelf -l file` | program headers, интерпретатор |
| `readelf -d file` | dynamic section (`INIT`) |
| `readelf -e file` | все секции (`.init`, `.fini`) |
| `ldd program` | используемые разделяемые библиотеки |
| `strace program` | системные вызовы |
| `gdb` + `info files` | секции и точка входа |

### Опции сборки

| Опция | Назначение |
| --- | --- |
| `make -jN` | N параллельных задач |
| `make O=dir` | вывод в каталог |
| `make V=1` | подробный вывод |
| `make M=dir` | внешние модули |
| `make ARCH=arm64 defconfig` | конфиг для архитектуры |
| `make menuconfig` / `nconfig` | меню конфигурации |
| `CROSS_COMPILE=prefix-` | кросс-компилятор |
| `make modules_install` | установка модулей |

### Файлы и разделы

| Объект | Роль |
| --- | --- |
| `crt1.o` | `_start` — точка входа |
| `crti.o` | прологи `.init` / `.fini` |
| `crtn.o` | эпилоги `.init` / `.fini` |
| `.interp` | путь к динамическому линковщику |
| `.init` / `.fini` | конструктор / деструктор |
| `built-in.o` | слитые объекты директории |
| `System.map` | таблица символов ядра |
| `bzImage` | сжатый образ ядра |

### Память: ключевые факты

| Механизм | Суть |
| --- | --- |
| `memblock` | раннее распределение до обычных аллокаторов |
| `memblock_add` / `memblock_reserve` | регионы `memory` / `reserved` |
| fix-mapped | постоянные виртуальные адреса (IDT, APIC) |
| `fix_to_virt` / `virt_to_fix` | индекс ↔ адрес |
| `ioremap` | физический I/O-адрес → виртуальный |
| `request_region` | регистрация диапазона I/O-портов |
| `request_mem_region` | регистрация диапазона памяти |
| `/proc/ioports`, `/proc/iomem` | карты портов и памяти |
| kmemcheck | valgrind для ядра: ловит неиниц. память |
| `kmemcheck=0/1/2` | выкл / вкл / one-shot |

### Патчи: коротко

```bash
git checkout -b my-fix
git commit -s -v              # Signed-off-by + diff в редакторе
git format-patch master       # файл .patch
./scripts/get_maintainer.pl -f file.c
git send-email --to ... --cc ... file.patch
```

Формат темы: `[PATCH] subsystem: краткое описание`, строки ≤ 80
символов, при правках — `[PATCH v2]` и changelog.
