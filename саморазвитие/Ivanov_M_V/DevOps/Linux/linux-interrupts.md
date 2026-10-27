# Прерывания и обработка прерываний в Linux

> Конспективное изложение глав книги `linux-insides`
> (`linux-interrupts-1`–`linux-interrupts-10`, полностью на английском —
> переведены). Теория, IDT, стеки, обработчики исключений, внешние
> прерывания, отложенная обработка и практика на реальном драйвере.

---

## 📌 Оглавление

- [Что такое прерывание](#что-такое-прерывание)
  - [Контроллеры прерываний: PIC и APIC](#контроллеры-прерываний-pic-и-apic)
  - [Три шага обработки](#три-шага-обработки)
  - [Типы прерываний и исключений](#типы-прерываний-и-исключений)
  - [Faults, traps, aborts](#faults-traps-aborts)
  - [Маскируемые и немаскируемые](#маскируемые-и-немаскируемые)
- [IDT и номера векторов](#idt-и-номера-векторов)
  - [Устройство IDT](#устройство-idt)
  - [Типы гейтов](#типы-гейтов)
  - [Поля записи IDT](#поля-записи-idt)
  - [IST — Interrupt Stack Table](#ist--interrupt-stack-table)
- [Стеки для прерываний](#стеки-для-прерываний)
  - [Стек потока и IRQ-стек](#стек-потока-и-irq-стек)
  - [Защитная канарейка стека](#защитная-канарейка-стека)
  - [Кадр прерывания на стеке](#кадр-прерывания-на-стеке)
- [Ранняя настройка IDT и стеков](#ранняя-настройка-idt-и-стеков)
  - [Пустая IDT и настройка gs](#пустая-idt-и-настройка-gs)
  - [Ранние обработчики исключений](#ранние-обработчики-исключений)
  - [Канарейка в start_kernel](#канарейка-в-start_kernel)
  - [Запрет локальных прерываний](#запрет-локальных-прерываний)
  - [early_trap_init и установка гейтов](#early_trap_init-и-установка-гейтов)
- [Механизм обработки исключений idtentry](#механизм-обработки-исключений-idtentry)
  - [Назначение макроса](#назначение-макроса)
  - [Фиктивный error code](#фиктивный-error-code)
  - [Определение контекста: paranoid](#определение-контекста-paranoid)
  - [Исключение из пользовательского пространства](#исключение-из-пользовательского-пространства)
  - [Исключение из ядра](#исключение-из-ядра)
  - [Выход из обработчика](#выход-из-обработчика)
- [Обработчики #DB и #BP](#обработчики-db-и-bp)
- [Ранний обработчик page fault](#ранний-обработчик-page-fault)
- [trap_init: полная настройка IDT](#trap_init-полная-настройка-idt)
  - [Каталог гейтов исключений](#каталог-гейтов-исключений)
  - [used_vectors и fixmap](#used_vectors-и-fixmap)
  - [cpu_init и TSS](#cpu_init-и-tss)
- [Каталог обработчиков исключений](#каталог-обработчиков-исключений)
  - [DO_ERROR и do_error_trap](#do_error-и-do_error_trap)
  - [notify_die и цепочки уведомлений](#notify_die-и-цепочки-уведомлений)
  - [do_trap и сигналы](#do_trap-и-сигналы)
  - [Double fault](#double-fault)
  - [Device not available](#device-not-available)
  - [General protection](#general-protection)
- [NMI и специальные исключения](#nmi-и-специальные-исключения)
  - [Немаскируемое прерывание](#немаскируемое-прерывание)
  - [Bounds и MPX](#bounds-и-mpx)
  - [Ошибки FPU и SIMD](#ошибки-fpu-и-simd)
- [Внешние прерывания](#внешние-прерывания)
  - [IRQ, ISR и структура irq_desc](#irq-isr-и-структура-irq_desc)
  - [early_irq_init](#early_irq_init)
  - [Число NR_IRQS](#число-nr_irqs)
  - [Sparse IRQ и MSI](#sparse-irq-и-msi)
- [Не-ранняя инициализация IRQ](#не-ранняя-инициализация-irq)
  - [init_IRQ и vector_irq](#init_irq-и-vector_irq)
  - [native_init_IRQ и init_ISA_irqs](#native_init_irq-и-init_isa_irqs)
  - [Таблица irq_entries_start](#таблица-irq_entries_start)
  - [Spurious и cascade IRQ2](#spurious-и-cascade-irq2)
- [Отложенные прерывания](#отложенные-прерывания)
  - [Top half и bottom half](#top-half-и-bottom-half)
  - [Softirq](#softirq)
  - [Как запускаются softirq](#как-запускаются-softirq)
  - [Tasklets](#tasklets)
  - [Workqueues](#workqueues)
- [Практика: serial-драйвер 21285](#практика-serial-драйвер-21285)
  - [Инициализация модуля](#инициализация-модуля)
  - [Запрос IRQ-линии request_irq](#запрос-irq-линии-request_irq)
  - [request_threaded_irq и __setup_irq](#request_threaded_irq-и-__setup_irq)
  - [Путь прерывания до do_IRQ](#путь-прерывания-до-do_irq)
  - [Выход из прерывания](#выход-из-прерывания)
- [Шпаргалка](#шпаргалка)

---

## Что такое прерывание

**Прерывание (interrupt)** — событие, которое программное обеспечение или
железо создают, когда процессору нужно внимание CPU. Пример: нажатие клавиши
на клавиатуре. Упрощённо: у каждого периферийного устройства есть линия
прерывания к процессору, через которую оно сообщает о себе.

Прерывания могут произойти в любой момент, и операционная система обязана
обработать их немедленно.

### Контроллеры прерываний: PIC и APIC

Прерывания не поступают на CPU напрямую:

- **PIC** (Programmable Interrupt Controller) — старый чип, последовательно
  обрабатывает запросы нескольких устройств.
- **APIC** (Advanced Programmable Interrupt Controller) — современный
  контроллер из двух устройств:
  - **Local APIC** — в каждом ядре процессора, отвечает за конфигурацию
    прерываний конкретного CPU (таймер APIC, датчик температуры,
    локальные I/O-устройства);
  - **I/O APIC** — управление прерываниями в мультипроцессорной системе,
    распределяет внешние прерывания между ядрами.

### Три шага обработки

Когда произошло прерывание, ядро должно:

1. **Приостановить выполнение текущего процесса**;
2. **Найти обработчик прерывания** и передать ему управление;
3. **После завершения обработчика** возобновить прерванное выполнение.

Это базовый скелет процедуры; адреса обработчиков хранятся в таблице
**IDT** (Interrupt Descriptor Table).

### Типы прерываний и исключений

Процессор использует уникальный **номер вектора** — индекс в IDT.
Векторов от `0` до `255`; в коде ядра есть проверка
`BUG_ON((unsigned)n > 0xFF)`.

- Векторы `0`–`31` зарезервированы процессором под исключения и
  архитектурные прерывания;
- Векторы `32`–`255` — пользовательские, обычно назначаются внешним
  I/O-устройствам.

По происхождению прерывания делятся на два класса:

- **Внешние (аппаратные)** — поступают через Local APIC или через пины
  процессора, подключённые к нему;
- **Программные** — вызываются исключительным состоянием самого процессора
  (например, деление на ноль) или специальными инструкциями
  (например, выход из программы по `syscall`).

Прерывания возникают **асинхронно** (когда угодно), а исключения —
**синхронно** с выполнением программы.

### Faults, traps, aborts

Исключения классифицируются на три категории:

- **Fault** — сообщается **до** выполнения «виноватой» инструкции; если
  причину устранить, прерванную программу можно продолжить;
- **Trap** — сообщается **сразу после** выполнения инструкции; также
  позволяет продолжить программу;
- **Abort** — не всегда указывает точную инструкцию и **не позволяет**
  возобновить прерванную программу.

### Маскируемые и немаскируемые

- **Маскируемые** — можно заблокировать инструкциями `cli` и `sti`
  (x86_64: `native_irq_disable`/`native_irq_enable`, они меняют бит `IF`
  — Interrupt Flag);

- **Немаскируемые (NMI)** — всегда обрабатываются; обычно отображают
  сбой hardware.

Если исключения/прерывания происходят одновременно, процессор обрабатывает
их в порядке приоритетов (от высшего к низшему):

| Приоритет | Описание |
| --------- | -------- |
| 1 | Сброс и machine check (RESET) |
| 2 | Переключение задачи (T-бит в TSS) |
| 3 | Внешние вмешательства (FLUSH, SMI, INIT) |
| 4 | Брейкпоинты и debug-трапы |
| 5 | Немаскируемые прерывания (NMI) |
| 6 | Маскируемые аппаратные прерывания |
| 7 | Fault брейкпоинта кода |
| 8 | Fault при выборке инструкции (limit CS, page fault кода) |
| 9 | Fault при декодировании (длина > 15 байт, неверный опкод) |
| 10 | Fault при выполнении (overflow, bound, #GP, page fault данных…) |

---

## IDT и номера векторов

### Устройство IDT

IDT по структуре похожа на GDT (Global Descriptor Table), но вместо
дескрипторов её записи называются **гейтами**. Как и GDT, IDT — массив
записей: по 8 байт на x86 и по 16 байт на x86_64.

Отличия от GDT:

- Обязательного NULL-дескриптора первым элементом **нет** — можно
  загрузить IDT из нулей: так делает `setup_idt` при переходе в
  защищённый режим, выполняя `lidtl` для `{0, 0}`;

- Базовый адрес IDT должен быть выровнен по 8 байт (x86) или 16 байт
  (x86_64) и может лежать в любом месте линейного пространства адресов.

Базовый адрес хранится в регистре **IDTR** (48 бит: лимит + база —
`struct gdt_ptr { u16 len; u32 ptr; }`). Инструкции работы с ним:

- **LIDT** — загрузка IDTR из операнда;
- **SIDT** — чтение IDTR в операнд.

Процессор формирует индекс записи IDT умножением номера вектора на 16
(размер гейта в long mode) — так же, как call-адрес, только по номеру
исключения/прерывания.

### Типы гейтов

Три вида обработчиков:

- **Interrupt gate**;
- **Trap gate**;
- **Task gate**.

В x86_64 (long mode) поддерживаются только interrupt gate и trap gate.
Тип записи задаётся полем `Type` (значения `GATE_INTERRUPT`, `GATE_TRAP`,
`GATE_CALL`, `GATE_TASK`).

### Поля записи IDT

Запись IDT на x86_64 (16 байт):

- Биты `0–15` — младшая часть смещения entry point обработчика;
- Биты `16–31` — селектор сегмента (содержит entry point);
- **IST** — индекс стека из Interrupt Stack Table (см. ниже);
- **DPL** — Descriptor Privilege Level (уровень привилегий);
- **P** — флаг присутствия сегмента (Present);
- Биты `48–63` и `64–95` — остальные части адреса обработчика;
- Биты `96–127` — зарезервированы процессором.

В ядре IDT — массив записей `gate_desc`:

```c
extern gate_desc idt_table[];

struct gate_struct64 {
        u16 offset_low;
        u16 segment;
        unsigned ist : 3, zero0 : 5, type : 5, dpl : 2, p : 1;
        u16 offset_middle;
        u32 offset_high;
        u32 zero1;
} __attribute__((packed));
```

### IST — Interrupt Stack Table

**IST** — новый механизм x86_64, альтернатива старому переключению стеков.
При включении для конкретного прерывания (по полю IST в записи IDT)
процессор **безусловно** переключается на указанный стек. Механизм
предоставляет до семи IST-указателей в **TSS** (Task State Segment).

Используемые IST-стеки:

```c
#define DOUBLEFAULT_STACK 1
#define NMI_STACK 2
#define DEBUG_STACK 3
#define MCE_STACK 4
```

Гейты с IST регистрируются функцией `set_intr_gate_ist`:

```c
set_intr_gate_ist(X86_TRAP_NMI, &nmi, NMI_STACK);
set_intr_gate_ist(X86_TRAP_DF, &double_fault, DOUBLEFAULT_STACK);
```

Где `&nmi` и `&double_fault` — адреса entry points обработчиков
(`asmlinkage void nmi(void);`, `asmlinkage void double_fault(void);`).

---

## Стеки для прерываний

### Стек потока и IRQ-стек

Каждый активный поток в x86_64 имеет собственный стек размером
`THREAD_SIZE = PAGE_SIZE << THREAD_SIZE_ORDER` (`PAGE_SHIFT = 12`,
`THREAD_SIZE_ORDER = 2 + KASAN_STACK_ORDER`).

- С `CONFIG_KASAN` — 32768 байт, без него — 16384 байта;
- Пока поток в user-space, ядерный стек пуст (внизу только `thread_info`);
- Дополнительно у **каждого CPU** есть специальные per-cpu стеки, активные
  только когда ядро выполняется на этом процессоре.

**IRQ-стек** для внешних аппаратных прерываний:

`IRQ_STACK_SIZE = PAGE_SIZE << IRQ_STACK_ORDER` — то есть 16 КБ.
Представляется объединением:

```c
union irq_stack_union {
        char irq_stack[IRQ_STACK_SIZE];
        struct {
                char gs_base[40];
                unsigned long stack_canary;
        };
};
```

- **`gs_base`** — регистр `gs` всегда указывает на начало irq-стека; в
  x86_64 `gs` разделяется между per-cpu областью и canary (сегментация в
  long mode отменена, но базу `fs`/`gs` задают через MSR). База задаётся
  в `startup_64` записью MSR `MSR_GS_BASE` (`0xc0000101`);
- **`stack_canary`** — защита стека от перезаписи; GCC требует фиксированное
  смещение от базы `gs` — 40 для x86_64 (и 20 для x86).

`irq_stack_union` — первый элемент в per-cpu области (`System.map`):
`irq_stack_union` на `0x0`, далее `exception_stacks` на `0x4000`,
`gdt_page` на `0x9000`.

Рядом объявлены `irq_stack_ptr` (вершина irq-стека) и `irq_count`
(находимся ли мы уже на стеке). `irq_stack_ptr` инициализируется
в `setup_per_cpu_areas` для каждого CPU:
`per_cpu(irq_stack_ptr, cpu) = per_cpu(irq_stack_union.irq_stack, cpu) +
IRQ_STACK_SIZE`.

### Защитная канарейка стека

Стек-канарейка (stack canary) — значение-маркер, по которому видно, что
стек перезаписали. Для irq-стека значение лежит в `irq_stack_union`
по смещению 40 (проверяется `BUILD_BUG_ON`).

### Кадр прерывания на стеке

При прерывании/исключении процессор кладёт на стек (размер pushes в
64-битном режиме фиксирован — 8 байт):

```text
+---------------+
|      SS       | +40
|      RSP      | +32
|     RFLAGS    | +24
|      CS       | +16
|      RIP      | +8
|   Error code  | +0  (если есть, иначе добавляется фиктивный)
+---------------+
```

Дальнейшие шаги процессора при входе в обработчик:

1. Если поле IST гейта ≠ 0 — загрузить IST-указатель в `rsp`;
2. Положить error code (настоящий или фиктивный — для консистентности
   стека);
3. Загрузить селектор из гейта в `cs`, проверив бит `L` (64-битный код);
4. Загрузить смещение из гейта в `rip` — начало обработчика;
5. Обработчик выполняется и завершается инструкцией **`iret`**, которая
   восстанавливает `ss:rsp`, `rflags`, `cs`, `rip` прерванной задачи.

---

## Ранняя настройка IDT и стеков

### Пустая IDT и настройка gs

Самое раннее место, связанное с прерываниями, — `setup_idt` в
`arch/x86/boot/pm.c`, вызывается перед переходом в защищённый режим:

```c
void go_to_protected_mode(void)
{
        ...
        setup_idt();
        ...
}
```

Таблица заполняется нулями: обрабатывать что-либо ещё рано. Отдельной
структуры `idt_ptr` нет — используется та же `gdt_ptr`, что и для
`GDTR`/`IDTR`.

Далее — `startup_64` (`arch/x86/kernel/head_64.S`): строятся
identity-таблицы страниц, обновляется ранний GDT, и настраивается `gs`
записью в MSR `MSR_GS_BASE` значения `edx:eax`:

Конкретно: `movl $MSR_GS_BASE,%ecx`, загрузка `initial_gs(%rip)` в
`%eax`/`%edx` и `wrmsr`. `initial_gs` указывает на `irq_stack_union`: макрос линковщика
`INIT_PER_CPU(x)` задаёт `init_per_cpu__##x = x + __per_cpu_load`, то есть
базовый адрес `init_per_cpu__irq_stack_union` равен
`irq_stack_union + __per_cpu_load` (видно в `System.map` рядом с
`__per_cpu_load`). Итог: `gs` указывает на начало irq-стека.

### Ранние обработчики исключений

В конце архитектурного раннего кода (`x86_64_start_kernel`) ранняя IDT
заполняется entry points обработчиков:

Цикл `for (i = 0; i < NUM_EXCEPTION_VECTORS; i++)` ставит
`set_intr_gate(i, early_idt_handler_array[i])`, затем `load_idt`.

`early_idt_handler_array` — массив entry points в `head_64.S`
(`NUM_EXCEPTION_VECTORS = 32`, `EARLY_IDT_HANDLER_SIZE = 9` байт на
каждый, создаётся через `.rept`), ведёт к `early_make_pgtable` и далее к
`early_idt_handler_common`.

### Канарейка в start_kernel

В `start_kernel` (`init/main.c`) вызывается `boot_init_stack_canary`
(при `CONFIG_CC_STACKPROTECTOR`):

1. `BUILD_BUG_ON(offsetof(union irq_stack_union, stack_canary) != 40)`
   проверяет, что `stack_canary` лежит по смещению 40;
2. Новое значение вычисляется из случайного числа (`get_random_bytes`)
   и Time Stamp Counter: к случайному значению прибавляется `tsc`
   и `tsc << 32`;
3. Запись значения в стек текущего процессора:

   ```c
   this_cpu_write(irq_stack_union.stack_canary, canary);
   ```

### Запрет локальных прерываний

Далее в `start_kernel` вызывается макрос `local_irq_disable`
(`include/linux/irqflags.h`):

- `raw_local_irq_disable` сворачивается к инструкции `cli`;
- при `CONFIG_TRACE_IRQFLAGS_SUPPORT` дополнительно вызывается
  `trace_hardirqs_off` — подсистема **lockdep** ведёт статистику
  событий включения/выключения hard/soft IRQ (видна в `/proc/lockdep`
  при `CONFIG_DEBUG_LOCKDEP`);
- обратный макрос `local_irq_enable` включает прерывания инструкцией
  `sti`.

Важно: «локальные» — потому что глобальный `cli` на всех процессорах из
ядра убрали; есть только `local_irq_{enable,disable}` для текущего CPU.
После выключения прерываний выставляется флаг `early_boot_irqs_disabled =
true` — он используется, например, в `smp_call_function_many` для проверки
возможного deadlock.

### early_trap_init и установка гейтов

В `setup_arch` первая функция, связанная с исключениями, —
`early_trap_init` (`arch/x86/kernel/traps.c`):

```c
void __init early_trap_init(void)
{
        set_intr_gate_ist(X86_TRAP_DB, &debug, DEBUG_STACK);
        set_system_intr_gate_ist(X86_TRAP_BP, &int3, DEBUG_STACK);
#ifdef CONFIG_X86_32
        set_intr_gate(X86_TRAP_PF, page_fault);
#endif
        load_idt(&idt_descr);
}
```

Три функции установки (все в `arch/x86/include/asm/desc.h`):

- `set_intr_gate_ist(n, addr, ist)` — гейт с DPL 0; внутри
  `BUG_ON((unsigned)n > 0xFF)` и `_set_gate(n, GATE_INTERRUPT, addr, 0,
  ist, __KERNEL_CS)`;
- `set_system_intr_gate_ist` отличается только четвёртым параметром —
  DPL `0x3` (низший уровень привилегий) вместо `0x0`: это позволяет
  вызывать гейт (например `int3`) из пользовательского пространства;
- `_set_gate` собирает запись через `pack_gate` (заполняет
  `offset_low/segment/ist/p/dpl/type/offset_middle/offset_high`) и пишет
  её в `idt_table` копированием (`native_write_idt_entry` — `memcpy`
  одной записи, разделяемой между таблицами по указателю).

Параметры всех `set_*_gate*`: номер вектора, адрес обработчика,
индекс IST.

---

## Механизм обработки исключений idtentry

### Назначение макроса

Обработчик состоит из двух частей:

1. **Общая** (одна для всех) — сохранить регистры на стеке, переключиться
   на ядерный стек, если исключение из user-space, передать управление
   дальше;
2. **Конкретная** — работа, специфичная для исключения (page fault ищет
   страницу, invalid opcode шлёт `SIGILL` и т.д.).

Общая часть генерируется ассемблерным макросом `idtentry`
(`arch/x86/entry/entry_64.S`):

```assembly
.macro idtentry sym do_sym has_error_code:req paranoid=0 shift_ist=-1
ENTRY(\sym)
        ...
        call    \do_sym
        ...
END(\sym)
.endm
```

Параметры:

- `sym` — глобальный символ, entry point исключения;
- `do_sym` — символ вторичного (C) обработчика;
- `has_error_code` — есть ли у исключения error code;
- `paranoid` — способ проверки, откуда пришли (см. ниже);
- `shift_ist` — работает ли исключение на IST-стеке.

Например: `idtentry debug do_debug has_error_code=0 paranoid=1
shift_ist=DEBUG_STACK` — то же для `int3`/`do_int3`.

### Фиктивный error code

Если у исключения нет error code (`has_error_code` = 0), макрос
кладёт на стек `-1` (`.ifeq` / `pushq $-1`).

Это не только «заглушка» для выравнивания стека: `-1` — также
неверный номер системного вызова, поэтому логика перезапуска syscall
не срабатывает.

Затем выделяется место под общие регистры (`pt_regs`) — `ALLOC_PT_GPREGS_ON_STACK`:
15×8 байт.

### Определение контекста: paranoid

Нужно понять, исключение из user-space или из ядра. Быстрый способ —
проверить младшие биты `cs` (CPL): 3 — пользователь, 0 — ядро —
`testl $3,CS(%rsp)` / `jnz userspace`.

Но в «супер-атомных» контекстах (NMI/MCE/DEBUG) этот способ ненадёжен:
NMI может сработать между записью `cs` на стек и выполнением `swapgs`.
Тогда единственный безопасный (но медленный) способ — чтение MSR:
`movl $MSR_GS_BASE,%ecx`, `rdmsr`, `testl %edx,%edx`, `js 1f`.

`MSR_GS_BASE` хранит указатель на начало per-cpu области. Из
пользовательского пространства задать отрицательное значение `gs`
нельзя, а адрес ядра (`0xffff880000000000` и выше) даёт старшие биты
`= 1`, то есть «отрицательное» значение в `%edx`. Значит `js` — мы в
ядре.

### Исключение из пользовательского пространства

При `paranoid=1` сначала проверяется `cs`; если пользователь — вызывается
`error_entry`, который выполняет `SAVE_C_REGS 8` (общие регистры в
`pt_regs`) и, главное, инструкцию **`SWAPGS`**: меняются местами значения
`MSR_KERNEL_GS_BASE` и `MSR_GS_BASE`, после чего `%gs` указывает на базу
ядерных структур.

Далее `%rdi` получает `%rsp`, вызывается `sync_regs`, и результат
(`%rax`) загружается в `%rsp`. `sync_regs` копирует сохранённые
регистры в `pt_regs` настоящего
ядерного стека процессора (адрес даёт `task_pt_regs(current)` =
`thread.sp0 - 1`) и возвращает новый `rsp` — обработчик работает в
настоящем контексте процесса.

Перед вызовом C-обработчика:

- `%rdi` ← указатель на `pt_regs` (первый аргумент по x86_64 ABI);
- `%rsi` ← error code (второй аргумент; если его нет — ноль), а в стек
  записывается `-1`, чтобы не перезапускать syscall;
- `call \do_sym` — вызов вторичного обработчика:

```c
dotraplinkage void do_debug(struct pt_regs *regs, long error_code);
dotraplinkage void notrace do_int3(struct pt_regs *regs, long error_code);
```

### Исключение из ядра

Если исключение произошло в ядре при `paranoid=1`, вызывается
`paranoid_entry` — медленная проверка через MSR:

```assembly
ENTRY(paranoid_entry)
        cld
        SAVE_C_REGS 8
        SAVE_EXTRA_REGS 8
        movl    $1, %ebx
        movl    $MSR_GS_BASE, %ecx
        rdmsr
        testl   %edx, %edx
        js      1f
        SWAPGS
        xorl    %ebx, %ebx
1:      ret
END(paranoid_entry)
```

`ebx = 1` — `swapgs` не выполнялся; `ebx = 0` — выполнялся (позже по нему
решают, нужен ли обратный `swapgs`). Далее сохраняется `cr2` в `%r12`
(NMI/исключение может вызвать page fault и испортить `cr2`), подготавливаются
`%rdi`/`%rsi` как выше, при `shift_ist != -1` корректируется указатель
IST-стека (`subq $EXCEPTION_STKSZ, CPU_TSS_IST(shift_ist)`) и вызывается
`call \do_sym`.

При `paranoid=0` используется быстрая проверка по `%cs`.

### Выход из обработчика

После C-обработчика макрос `idtentry` переходит к `error_exit`:
определяется прежний контекст (user/kernel), при необходимости выполняется
`SWAPGS`, регистры восстанавливаются, инструкция **`iret`** возвращает
управление прерванной задаче.

---

## Обработчики #DB и #BP

Ранние обработчики из `early_trap_init`:

- **`#DB` (debug)** — вектор `1`, error code нет. Возникает при
  отладочных событиях, например при попытке изменить debug-регистр
  (появились с Intel 80386). Debug-регистры доступны только в
  привилегированном режиме, поэтому для `#DB` используется
  `set_intr_gate_ist` (DPL 0), а не системный гейт.
- **`#BP` (breakpoint)** — вызывается инструкцией `int 3`. В отличие от
  `#DB`, может сработать **в пользовательском пространстве**:

```c
int main() {
    int i;
    while (i < 6) {
        printf("i equal to: %d\n", i);
        __asm__("int3");
        ++i;
    }
}
```

Без отладчика программа завершается с `Trace/breakpoint trap`, а под
`gdb` получает `SIGTRAP` и продолжает выполнение после `c` — так работают
software-брейкпоинты.

Оба обработчика (`debug`, `int3`) объявлены в `traps.h` с директивой
`asmlinkage` (параметры берутся со стека — соглашение для функций,
вызываемых из ассемблера) и определены макросом `idtentry` в
`entry_64.S` с `paranoid=1 shift_ist=DEBUG_STACK`.

---

## Ранний обработчик page fault

Ранний `#PF` ставит `early_trap_pf_init`:

`X86_TRAP_PF` = `14` — гейт ставит `early_trap_pf_init`
(`set_intr_gate(X86_TRAP_PF, page_fault)` под `CONFIG_X86_64`).
Обработчик определён как:

Обработчик объявлен как `trace_idtentry page_fault do_page_fault
has_error_code=1`: при `CONFIG_TRACING` даёт дополнительную
trace-версию, без него сворачивается к обычному `idtentry`.

C-обработчик `do_page_fault` (`arch/x86/mm/fault.c`) объявлен как
`dotraplinkage void notrace do_page_fault(struct pt_regs *regs,
unsigned long error_code)` и читает `address = read_cr2()`.

- **`cr2`** — линейный адрес, вызвавший page fault;
- стандартный каркас «context tracking» для RCU: `prev_state =
  exception_enter();` ... `exception_exit(prev_state);`
  (`exception_enter/exit` сообщают подсистеме context tracking, что
  процессор перешёл из user в kernel: `IN_KERNEL`/`IN_USER`).

Далее вызывается `__do_page_fault`:

1. Проверка активности `kmemcheck` (детектор неинициализированной памяти —
   сам может вызвать page fault), `prefetchw` для скрытия задержки
   доступа к памяти;
2. Определение пространства faults: `fault_in_kernel_space(address)`
   возвращает `address >= TASK_SIZE_MAX`, где
   `TASK_SIZE_MAX = (1UL << 47) - PAGE_SIZE = 0x00007ffffffff000`;

3. Макросы оптимизации `likely(x)`/`unlikely(x)` (`__builtin_expect`)
   подсказывают компилятору ожидаемую ветку — код редкой ветки
   компилируется в дальний участок;
4. Далее — разбор причины (kmemcheck, spurious, kprobes…); подробности
   разбираются в главе про управление памятью.

---

## trap_init: полная настройка IDT

В `start_kernel` сразу после `setup_arch` вызывается `trap_init`
(`arch/x86/kernel/traps.c`) — инициализация остальных обработчиков.

### Каталог гейтов исключений

```c
set_intr_gate(X86_TRAP_DE, divide_error);
set_intr_gate_ist(X86_TRAP_NMI, &nmi, NMI_STACK);

set_system_intr_gate(X86_TRAP_OF, &overflow);
set_intr_gate(X86_TRAP_BR, bounds);
set_intr_gate(X86_TRAP_UD, invalid_op);
set_intr_gate(X86_TRAP_NM, device_not_available);

set_intr_gate_ist(X86_TRAP_DF, &double_fault, DOUBLEFAULT_STACK);

set_intr_gate(X86_TRAP_OLD_MF, &coprocessor_segment_overrun);
set_intr_gate(X86_TRAP_TS, &invalid_TSS);
set_intr_gate(X86_TRAP_NP, &segment_not_present);
set_intr_gate_ist(X86_TRAP_SS, &stack_segment, STACKFAULT_STACK);
set_intr_gate(X86_TRAP_GP, &general_protection);
set_intr_gate(X86_TRAP_SPURIOUS, &spurious_interrupt_bug);
set_intr_gate(X86_TRAP_MF, &coprocessor_error);
set_intr_gate(X86_TRAP_AC, &alignment_check);

#ifdef CONFIG_X86_MCE
        set_intr_gate_ist(X86_TRAP_MC, &machine_check, MCE_STACK);
#endif
set_intr_gate(X86_TRAP_XF, &simd_coprocessor_error);
```

Что означают исключения:

- `#DE` — ошибка деления;
- `#OF` — overflow при `INTO`;
- `#BR` — вышли за границы массива (`BOUND`);
- `#UD` — недопустимый опкод;
- `#NM` — обращение к FPU при установленном флаге `EM` в `cr0`;
- `#DF` — второй exception во время обработки первого (не удалось
  обработать их последовательно);
- `#CSO` — overrun сопроцессора (старые процессоры больше не выдают);
- `#TS` — ошибка TSS;
- `#NP` — сегмент/gate не присутствует (бит P = 0);
- `#SS` — ошибка стека;
- `#GP` — нарушение защиты (привилегии, limit, обращение к IDT не через
  gate…);
- Spurious interrupt — нежелательное аппаратное прерывание;
- `#MF` — ошибка x87 FPU;
- `#AC` — невыровненный доступ при включённой проверке выравнивания;
- `#MC` — machine check (внутренняя/шинная ошибка, при
  `CONFIG_X86_MCE`);
- `#XF` — SIMD-исключение SSE/SSE2/SSE3 (6 классов: invalid operation,
  divide-by-zero, denormal, overflow, underflow, precision).

### used_vectors и fixmap

Заполняется битмап `DECLARE_BITMAP(used_vectors, NR_VECTORS)`:
циклом `for (i = 0; i < FIRST_EXTERNAL_VECTOR; i++) set_bit(i,
used_vectors)` — `FIRST_EXTERNAL_VECTOR = 0x20`, первые 32 вектора
уже заняты.

При `CONFIG_IA32_EMULATION` добавляется вектор `0x80`:
`set_system_intr_gate(IA32_SYSCALL_VECTOR, ia32_syscall)` и
`set_bit(IA32_SYSCALL_VECTOR, used_vectors)`.

IDT маппится в fixmap-область только для чтения: `__set_fixmap
(FIX_RO_IDT, __pa_symbol(idt_table), PAGE_KERNEL_RO)` и
`idt_descr.address = fix_to_virt(FIX_RO_IDT)`.

### cpu_init и TSS

`cpu_init` инициализирует всё per-cpu состояние:

- сначала `wait_for_master_cpu`, `cr4_init_shadow`, `load_ucode_ap`;
- берутся TSS и оригинальные IST текущего процессора
  (`t = &per_cpu(cpu_tss, cpu); oist = &per_cpu(orig_ist, cpu);`);
- из `cr4` снимаются биты `VME|PVI|TSD|DE` (vm86, виртуальные прерывания,
  запрет RDTSC для низших привилегий, debug-расширение);
- перезагружаются GDT и IDT: `switch_to_new_gdt`, `loadsegment(fs, 0)`,
  `load_current_idt`;

- заполняются IST-стеки для всех `N_EXCEPTION_STACKS` (4) исключений:
  из `per_cpu(exception_stacks, cpu)` отсчитываются `exception_stack_sizes`
  вперёд, и каждый `oist->ist[v]`/`t->x86_tss.ist[v]` указывает на
  вершину своего стека (для `DEBUG_STACK-1` также заполняется
  `per_cpu(debug_stack_addr, cpu)`); при повторном вызове на другой CPU
  IST уже инициализированы;
- дескриптор TSS записывается в GDT (`set_tss_desc`) и загружается
  инструкцией `ltr` (`load_TR_desc`).

В конце `trap_init` `#DB` и `#BP` ставятся **повторно** (плюс копия в
`nmi_idt_table`): в `early_trap_init` TSS ещё не был готов, а после
`cpu_init` готов.

---

## Каталог обработчиков исключений

### DO_ERROR и do_error_trap

Восемь «простых» исключений генерируются макросом `DO_ERROR`:

```c
DO_ERROR(X86_TRAP_DE,     SIGFPE,  "divide error",                divide_error)
DO_ERROR(X86_TRAP_OF,     SIGSEGV, "overflow",                    overflow)
DO_ERROR(X86_TRAP_UD,     SIGILL,  "invalid opcode",              invalid_op)
DO_ERROR(X86_TRAP_OLD_MF, SIGFPE,  "coprocessor segment overrun",
        coprocessor_segment_overrun)
DO_ERROR(X86_TRAP_TS,     SIGSEGV, "invalid TSS",                 invalid_TSS)
DO_ERROR(X86_TRAP_NP,     SIGBUS,  "segment not present",
        segment_not_present)
DO_ERROR(X86_TRAP_SS,     SIGBUS,  "stack segment",               stack_segment)
DO_ERROR(X86_TRAP_AC,     SIGBUS,  "alignment check",             alignment_check)
```

Параметры: номер вектора, сигнал для прерванного процесса, описание,
entry point. Макрос (через конкатенацию `##` в GCC) создаёт функцию
`do_##name(regs, error_code)`, которая просто вызывает
`do_error_trap(regs, error_code, str, trapnr, signr)`.

`do_error_trap` завёрнут в `exception_enter/exception_exit`, затем:
`notify_die(DIE_TRAP, str, ...)`; если вернулось не `NOTIFY_STOP` —
`conditional_sti(regs)` и `do_trap(trapnr, signr, str, regs,
error_code, fill_trap_info(...))`.

### notify_die и цепочки уведомлений

**Notifier chains** — простые односвязные списки, механизм уведомления
подсистем о событиях (USB hotplug, memory hotplug, перезагрузка, panic,
oops, NMI). Подсистема регистрирует `notifier_block` через
`notifier_chain_register`, событие рассылается через
`notifier_call_chain`.

`notify_die` заполняет структуру `die_args` (регистры, строка, номер
трапа, сигнал) и рассылает её по атомарной цепочке
`atomic_notifier_call_chain(&die_chain, val, &args)` (голова —
`static ATOMIC_NOTIFIER_HEAD(die_chain)`).

Если вернулось `NOTIFY_STOP` — обработка прекращена. Иначе
`conditional_sti` включает прерывания, если они были включены до
исключения (`regs->flags & X86_EFLAGS_IF`).

### do_trap и сигналы

`do_trap` сначала пробует `do_trap_no_signal`:

- Из **user-space** (и не v8086, которого в long mode нет) — возврат `-1`,
  обработка продолжается;
- Из **ядра** — пытаемся исправить: `fixup_exception(regs)`, и если
  не удалось — записать `error_code`/`trap_nr` в `thread` и вызвать
  `die(str, regs, error_code)`. `die` печатает стек, регистры, модули —
  это **kernel oops**.

Если исключение из user-space, процессу отправляется сигнал
(`SIGFPE`/`SIGILL`/`SIGSEGV`/`SIGBUS`): в `thread` сохраняются
`error_code` и `trap_nr`, при `show_unhandled_signals` печатается
`printk` с ratelimit о непойманном сигнале (имя, pid, ip, sp, error
code), затем `force_sig_info(signr, info ?: SEND_SIG_PRIV, tsk)`.

### Double fault

`#DF` работает на собственном IST-стеке `DOUBLEFAULT_STACK` (=1).
Обработчик `double_fault`:

1. Особый случай — non-IST fault на `espfix64`-стеке (проблема
   восстановления 16-битного `ss` при `iret`): стек подделывается так,
   что выглядит как `#GP`, и управление уходит `general_protection`;
2. Обычный случай:

```c
ist_enter(regs);
tsk->thread.error_code = error_code;
tsk->thread.trap_nr = X86_TRAP_DF;
#ifdef CONFIG_DOUBLEFAULT
        df_debug(regs, error_code);   /* pid, регистры */
#endif
for (;;)
        die(str, regs, error_code);   /* бесконечный oops */
```

Double fault практически всегда фатален.

### Device not available

`#NM` возникает, когда:

- выполнение x87-инструкции при флаге `EM` в `cr0`;
- выполнение `wait`/`fwait` при `MP` и `TS`;
- выполнение x87/MMX/SSE при `TS` (и `EM` = 0).

Обработчик `do_device_not_available`:

- `BUG_ON(use_eager_fpu())` — FPU не должен грузиться при каждом
  переключении задач (не eager);
- если `EM` установлен (математического сопроцессора нет) — программная
  эмуляция: `conditional_sti(regs)` и `math_emulate(&info)` с
  `info.regs = regs`;
- иначе состояние FPU лениво восстанавливается из сохранённой копии
  (`fpu__restore(&current->thread.fpu)`) и инструкции FPU снова
  доступны.

### General protection

`#GP` — класс нарушений защиты: превышение limit сегмента, загрузка
селектора системного сегмента в `ss/ds/es/fs/gs`, нарушение привилегий
и т.д.

`do_general_protection`: сначала `conditional_sti(regs)`; в v8086-режиме
(`handle_vm86_fault`) — его в long mode нет. Далее, если исключение из
**ядра**: `fixup_exception(regs)` при наличии fixup-таблицы, иначе
запись `error_code`/`trap_nr` в `thread` и
`notify_die(DIE_GPF, "general protection fault", ...)`; при
`NOTIFY_STOP` не будет — `die("general protection fault", ...)`.

Из **user-space** процессу отправляется `SIGSEGV` (с опциональным
`pr_info` о непойманном сигнале): `force_sig_info(SIGSEGV,
SEND_SIG_PRIV, tsk)`.

---

## NMI и специальные исключения

### Немаскируемое прерывание

**NMI** — аппаратное прерывание, которое нельзя игнорировать стандартной
маскировкой. Источники:

- внешнее железо подаёт сигнал на NMI-пин процессора;
- сообщение на системной шине или APIC-шине с режимом доставки `NMI`.

Процессор обрабатывает его немедленно по вектору `2`
(`set_intr_gate_ist(X86_TRAP_NMI, &nmi, NMI_STACK)`). В отличие от
остальных, NMI-обработчик **не** определён макросом `idtentry` — у него
собственный entry point `ENTRY(nmi)` в `entry_64.S`.

Главная сложность — **вложенные NMI**:

- пока обрабатывается первый NMI, второй формально заблокирован, но если
  внутри NMI случится page fault/брейкпоинт, их `iret` снова разрешит
  NMI — и новый NMI затрёт стек предыдущего;
- решение: на стеке создаётся флаг-маркер (кладётся `1` = «NMI
  выполняется») плюс **две копии** исходного кадра — «сохранённая» и
  «копируемая». Вложенное NMI модифицирует только копию, а первый NMI
  по маркеру понимает, что надо повторить обработку.

Далее стандартные шаги: `pushq $-1`, `ALLOC_PT_GPREGS_ON_STACK`,
`call paranoid_entry` (проверка MSR + возможный `swapgs`, `%ebx` =
индикатор), сохранение `cr2` в `%r12`:

```assembly
movq    %cr2, %r12
movq    %rsp, %rdi
movq    $-1, %rsi
call    do_nmi
```

После `do_nmi`: восстановление `cr2` при порче, обратный `swapgs` по
`%ebx`, восстановление регистров, очистка маркера и `iret`.

C-обработчик `do_nmi` (`arch/x86/kernel/nmi.c`):

- `nmi_nesting_preprocess(regs)`: если мы на debug-стеке
  (`is_debug_stack(regs->sp)`) — `debug_stack_set_zero()` и флаг
  `update_debug_stack`, чтобы отладочный стек не переполнился;
- `nmi_enter`/`nmi_exit` — обновление `lockdep_recursion`,
  preempt-count и уведомление RCU;
- `default_do_nmi`:
  - сравнение `regs->ip` с `last_nmi_rip` (защита от «поглощения»
    повторяющихся NMI);
  - обработка CPU-специфичных NMI: `nmi_handle(NMI_LOCAL, …)`;
  - внешних по причине: `x86_platform.get_nmi_reason()` →
    `NMI_REASON_SERR` (`pci_serr_error`) или `NMI_REASON_IOCHK`
    (`io_check_error`).

### Bounds и MPX

`#BR` возникает, если индекс массива при `BOUND` вышел за границы.
Обработчик `do_bounds`: `notify_die` → `conditional_sti` → из ядра
`die`, из user-space `do_trap` с `SIGSEGV`. Если поддержка **MPX**
(Intel Memory Protection Extensions) включена, дополнительно проверяется
поле `BNDSTATUS` (`get_xsave_field_ptr(XSTATE_BNDCSR)`) — виноват ли MPX
в исключении.

### Ошибки FPU и SIMD

`#MF` (x87 FPU) и `#XF` (SIMD: SSE/SSE2/SSE3) обрабатываются почти
идентично — оба вызывают `math_error`, различаясь номером вектора:

```c
dotraplinkage void do_coprocessor_error(struct pt_regs *regs,
                                        long error_code)
{
        enum ctx_state prev_state;
        prev_state = exception_enter();
        math_error(regs, error_code, X86_TRAP_MF);
        exception_exit(prev_state);
}
/* do_simd_coprocessor_error аналогично с X86_TRAP_XF */
```

`math_error`: `notify_die`; из ядра — `fixup_exception` или `die`;
из user-space: `fpu__save(fpu)`, в `thread` сохраняются `trap_nr` и
`error_code`, заполняется `siginfo_t` (`si_signo = SIGFPE`,
`si_addr` — адрес из `uprobe_get_trap_addr(regs)`,
`si_code = fpu__exception_code(fpu, trapnr)`) и выполняется
`force_sig_info(SIGFPE, &info, task)`.

На этом `trap_init` завершён.

---

## Внешние прерывания

### IRQ, ISR и структура irq_desc

Аппаратные прерывания поступают по линиям **IRQ** (Interrupt Request
Line): устройство сообщает, что ему нужно внимание процессора. Ядро
временно прерывает текущую программу и вызывает **ISR** (Interrupt
Service Routine) — обработчик из таблицы векторов.

Почему нельзя просто слать сигнал, как с исключениями: прерывание часто
приходит **задолго** после того, как связанная с ним процесса уже не
работает.

Типы внешних прерываний: **I/O**, таймерные, межпроцессорные (IPI).

Обобщённый обработчик I/O-прерывания: сохранить номер IRQ и регистры на
ядерном стеке, подтвердить контроллеру, что IRQ принят, выполнить ISR
устройства, восстановить регистры и вернуться.

Основа кода управления прерываниями — структура **`irq_desc`**
(массив `irq_desc[]` отслеживает все источники IRQ):

- `irq_common_data` — данные, передаваемые в функции чипа;
- `status_use_accessors` — статус источника (комбинация значений из
  `enum` и макросов `include/linux/irq.h`);
- `kstat_irqs` — per-cpu статистика;
- `handle_irq` — высокоуровневый обработчик событий IRQ;
- `action` — список `irqaction` (ISR, вызываемых при IRQ);
- `irq_count` — счётчик срабатываний на линии;
- `depth` — `0`, если линия включена; >0 — выключалась;
- `last_unhandled`, `irqs_unhandled` — счётчики необработанных;
- `lock` — spinlock на доступ к дескриптору;
- `pending_mask` — ожидающие ребалансировки;
- `owner` — модуль-владелец (refcount).

### early_irq_init

Реализация (`kernel/irq/irqdesc.c`) зависит от `CONFIG_SPARSE_IRQ`.
Вариант **без** sparse IRQ: в начале берётся `node = first_online_node`
(первый online NUMA-узел, зависит от `MAX_NUMNODES`/`CONFIG_NODES_SHIFT`),
затем вызывается `init_irq_default_affinity()` (при `CONFIG_SMP`):
выделяет cpumask `irq_default_affinity` и заполняет её всеми
процессорами — это **SMP IRQ affinity**, позволяет назначать IRQ
конкретным CPU (`alloc_cpumask_var(..., GFP_NOWAIT)` +
`cpumask_setall`).

Затем печатается максимум дескрипторов: `printk(KERN_INFO "NR_IRQS:%d",
NR_IRQS)`.

### Число NR_IRQS

- Без `CONFIG_X86_IO_APIC` (старый PIC): `NR_IRQS = NR_IRQS_LEGACY = 16`;
- С APIC — зависит от числа процессоров и векторов:

```c
#define CPU_VECTOR_LIMIT               (64 * NR_CPUS)
#define NR_VECTORS                     256
#define IO_APIC_VECTOR_LIMIT           ( 32 * MAX_IO_APICS )
#define MAX_IO_APICS                   128

# define NR_IRQS                                       \
        (CPU_VECTOR_LIMIT > IO_APIC_VECTOR_LIMIT ?     \
                (NR_VECTORS + CPU_VECTOR_LIMIT)  :     \
                (NR_VECTORS + IO_APIC_VECTOR_LIMIT))
```

Пример: `NR_CPUS=8` → `CPU_VECTOR_LIMIT=512`,
`IO_APIC_VECTOR_LIMIT=4096` → `NR_IRQS:4352` (видно в `dmesg`).

Сам массив инициализируется статически (`struct irq_desc
irq_desc[NR_IRQS] __cacheline_aligned_in_smp`): для всех записей по
умолчанию `handle_bad_irq`, `depth = 1` и открытый spinlock `lock`.

`handle_bad_irq` — обработчик spurious/unhandled IRQ (hardware ещё не
настроен). Цикл инициализации для каждого дескриптора: `alloc_percpu` для
`kstat_irqs` (отдельная статистика на процессор — видна в 6-м столбце
`/proc/stat`), `alloc_masks`, `raw_spin_lock_init` +
`lockdep_set_class` (класс lock для валидатора блокировок),
`desc_set_defaults` (номер, `no_irq_chip`, `chip_data`, `handler_data`,
`msi_desc`, статусы, `handle_bad_irq`, `depth = 1`), `desc_smp_init`
(NUMA-узел и копирование `irq_default_affinity` в
`desc->irq_data.affinity`).

В конце — `return arch_early_irq_init();` → `arch_early_ioapic_init`:
если легаси-IRQ нет (`!nr_legacy_irqs()`) — `io_apic_irqs = ~0UL`
(все прерывания через APIC); для каждого I/O APIC выделяются
`alloc_ioapic_saved_registers`; для легаси-IRQ (16 штук) —
`alloc_irq_and_cfg_at(i, node)` с `cfg->vector = IRQ0_VECTOR + i` и
`cpumask_setall(cfg->domain)`.

### Sparse IRQ и MSI

Вариант **с** `CONFIG_SPARSE_IRQ` отличается: сначала
`arch_probe_nr_irqs()` считает предвыделенные IRQ. Здесь же — про
**MSI** (Message Signaled Interrupts): вместо фиксированного номера
линии устройство записывает сообщение по адресу RAM (= доставка через
Local APIC). MSI даёт 1/2/4/8/16/32 прерывания, **MSI-X** — до 2048.

`arch_probe_nr_irqs`: `nr_irqs = NR_IRQS`, но не больше
`NR_VECTORS * nr_cpu_ids`; далее `nr = (gsi_top + nr_legacy_irqs()) +
8 * nr_cpu_ids`, где `gsi_top` — база **GSI** (Global System Interrupt)
верхнего APIC из MP-таблицы. В `dmesg`: `NR_IRQS:4352 nr_irqs:488 16`.
Далее дескрипторы выделяются динамически (`alloc_desc(i, node, NULL)`,
`set_bit(i, allocated_irqs)`) и вставляются в radix-дерево
(`irq_insert_desc`). `IRQ_BITMAP_BITS` = `NR_IRQS` (без sparse) или
`NR_IRQS + 8196`.

---

## Не-ранняя инициализация IRQ

### init_IRQ и vector_irq

Сразу после `early_irq_init` в `start_kernel` вызывается `init_IRQ`
(`arch/x86/kernel/irqinit.c`). Инициализируется per-cpu массив
соответствий «вектор → номер IRQ»:

```c
DEFINE_PER_CPU(vector_irq_t, vector_irq) = {
         [0 ... NR_VECTORS - 1] = -1,
};
/* typedef int vector_irq_t[NR_VECTORS]; NR_VECTORS = 256 */
```

Заполнение легаси-векторов: цикл по `nr_legacy_irqs()` ставит
`per_cpu(vector_irq, 0)[IRQ0_VECTOR + i] = i`, где
`IRQ0_VECTOR = ((0x20 + 16) & ~15) = 0x30` — векторы `0x30`–`0x3f`
зарезервированы под ISA; `nr_legacy_irqs()` возвращает
`legacy_pic->nr_legacy_irqs` (максимум `NR_IRQS_LEGACY = 16`),
`legacy_pic` = `default_legacy_pic` (`arch/x86/kernel/i8259.c`).
Затем вызывается `x86_init.irqs.intr_init()`. Этот массив используется
в `do_IRQ`: `irq = __this_cpu_read(vector_irq[vector])`.

### native_init_IRQ и init_ISA_irqs

`x86_init.irqs` указывает: `.pre_vector_init = init_ISA_irqs`,
`.intr_init = native_init_IRQ`, `.trap_init = x86_init_noop`.

Структура **`irq_chip`** — дескриптор аппаратного чипа прерываний:
`name` (видно в последнем столбце `/proc/interrupts`), `irq_mask`,
`irq_ack`, `irq_startup`, `irq_shutdown` и т.д. Данные `irq_data`:
`mask`, `irq`, `hwirq` (hardware-номер в домене).

`init_ISA_irqs`: берёт `chip = legacy_pic->chip`; при
`CONFIG_X86_64`/`CONFIG_X86_LOCAL_APIC` вызывается `init_bsp_APIC()`
(выход, если APIC не найден/нет `smp_found_config`; далее
`clear_local_APIC()` и включение APIC — к `APIC_SPIV` добавляется
`APIC_SPIV_APIC_ENABLED` после очистки `APIC_VECTOR_MASK`); затем
`legacy_pic->init(0)` — инициализация Intel 8259
(`init_8259A`), и для каждого легаси-IRQ: `irq_set_chip_and_handler(i,
chip, handle_level_irq)`.

Далее в `native_init_IRQ` — `apic_intr_init` распределяет гейты для
**IPI** (межпроцессорных прерываний SMP) через `alloc_intr_gate(n,
addr)` — макрос делает `alloc_system_vector(n)` и `set_intr_gate(n,
addr)`.

`alloc_system_vector` помечает вектор в `used_vectors` и обновляет
`first_system_vector`. Проверка бита `test_bit` при компиляции:
`__builtin_constant_p(nr)` определяет, известно ли число на этапе
компиляции: константа → `constant_test_bit` (число подставляется
сразу), переменная → `variable_test_bit` (инструкция `btl`, значение
читается со стека). Разница — чистая оптимизация.

### Таблица irq_entries_start

Все свободные векторы от `FIRST_EXTERNAL_VECTOR` (0x20) до
`first_system_vector` (0xef) получают гейты:

```c
i = FIRST_EXTERNAL_VECTOR;
for_each_clear_bit_from(i, used_vectors, first_system_vector) {
        set_intr_gate(i, irq_entries_start +
                8 * (i - FIRST_EXTERNAL_VECTOR));
}
```

`irq_entries_start` — таблица entry stubs в `entry_64.S`:

```assembly
        .align 8
ENTRY(irq_entries_start)
    vector=FIRST_EXTERNAL_VECTOR
    .rept (FIRST_SYSTEM_VECTOR - FIRST_EXTERNAL_VECTOR)
        pushq   $(~vector+0x80)
    vector=vector+1
        jmp     common_interrupt
        .align  8
    .endr
END(irq_entries_start)
```

`.rept` повторяет тело `(0xef - 0x20) = 207` раз: на стек кладётся
номер вектора (в отрицательном представлении — положительные зарезервированы
под syscall) и выполняется прыжок на `common_interrupt`: `addq $-0x80,(%rsp)` и
`interrupt do_IRQ`.

Макрос `interrupt` сохраняет регистры, при необходимости делает `SWAPGS`,
увеличивает per-cpu `irq_count` и вызывает `do_IRQ`.

### Spurious и cascade IRQ2

Оставшиеся неиспользуемые векторы до `NR_VECTORS` закрываются
обработчиком spurious-прерываний (`spurious_interrupt`, при
`CONFIG_X86_LOCAL_APIC`).

В конце — настройка легаси-линии каскада, если нет ACPI/OpenFirmware
I/O APIC, а легаси-контроллер есть:

```c
if (!acpi_ioapic && !of_ioapic && nr_legacy_irqs())
        setup_irq(2, &irq2);
```

`irq2` — `irqaction` с именем `"cascade"` и `IRQF_NO_THREAD`: линия
`IRQ 2` соединяла два чипа 8259 (второй обслуживал линии 8–15).

Легаси-линии Intel 8259A:

| IRQ | Назначение |
| --- | ----------- |
| 0 | Системный таймер |
| 1 | Клавиатура |
| 2 | Каскадное подключение чипов |
| 3 / 4 | COM2+COM4 / COM1+COM3 |
| 5 / 7 | LPT2 / LPT1 |
| 6 | Контроллер дисковода |
| 8 | RTC |
| 12 | PS/2 мышь |
| 13 | Сопроцессор |
| 14 | Контроллер жёсткого диска |
| 9, 10, 11, 15 | Зарезервированы |

`setup_irq(irq, action)` → `irq_to_desc` + `__setup_irq` под замком
`chip_bus_lock`: создаёт поток обработчика (если задан `thread_fn`),
заполняет flags/`irqaction`. Информация видна в `/proc/irq/2/`
(`node`, `affinity_hint`, `spurious`) — на современных машинах обычно
нулевая, так как всё обслуживает APIC.

---

## Отложенные прерывания

### Top half и bottom half

Две конкурирующие требования:

1. Обработчик должен выполняться **быстро**;
2. Иногда ему нужно сделать **много работы**.

Решение — разделить обработку:

- **Top half** — главный обработчик делает только самое необходимое и
  завершается;
- **Bottom half** — отложенная часть выполняется позже, когда система
  меньше загружена.

Термин «bottom half» исторически относился к одной конкретной технологии,
но сейчас — обобщённое название всех способов отложенной обработки.
В Linux три типа: **softirq**, **tasklet**, **workqueue**.

### Softirq

На каждой CPU работает свой поток `ksoftirqd/n` (создаёт
`spawn_ksoftirqd`, зарегистрированный как `early_initcall`); видны в
`systemd-cgls -k` как `[ksoftirqd/0]`, `[ksoftirqd/1]`, …

Softirq определяются **статически на этапе компиляции**. Регистрация —
`open_softirq(nr, action)`: записывает `action` в
`softirq_vec[nr]` — массив `struct softirq_action { void (*action)(...); }`
размером `NR_SOFTIRQS`, выровненный по кеш-линии.

Десять типов softirq (видны в `/proc/softirqs`):

```c
enum
{
        HI_SOFTIRQ=0,       /* tasklet высокого приоритета */
        TIMER_SOFTIRQ,      /* таймеры */
        NET_TX_SOFTIRQ,     /* сеть: передача */
        NET_RX_SOFTIRQ,     /* сеть: приём */
        BLOCK_SOFTIRQ,      /* блочный слой */
        BLOCK_IOPOLL_SOFTIRQ,
        TASKLET_SOFTIRQ,    /* tasklet */
        SCHED_SOFTIRQ,      /* планировщик */
        HRTIMER_SOFTIRQ,    /* высокоточные таймеры */
        RCU_SOFTIRQ,        /* RCU */
        NR_SOFTIRQS         /* = 10 */
};
```

Активация — `raise_softirq(nr)`: `local_irq_save(flags)` (сохраняет
флаг `IF` и выключает прерывания), `raise_softirq_irqoff(nr)` ставит
бит в маске `__softirq_pending` локального процессора,
`local_irq_restore(flags)`. Затем:

```c
if (!in_interrupt())
        wakeup_softirqd();
```

Если мы не в контексте прерывания — будится поток `ksoftirqd`
(`wakeup_softirqd` → `wake_up_process`).

### Как запускаются softirq

`ksoftirqd` выполняет `run_ksoftirqd` → `__do_softirq`, которая читает
`__softirq_pending` и выполняет действия по каждому установленному биту.
Защита от голодания user-space — лимит времени: цикл `ffs(pending)`
продолжается, пока `time_before(jiffies, end)` (конец — `jiffies +
MAX_SOFTIRQ_TIME`), `!need_resched()` и не исчерпан `max_restart`
(до 10 перезапусков); после — повторная проверка `pending`.

Проверка отложенных работ также происходит в конце обработки
аппаратного прерывания: `do_IRQ` → `exiting_irq` → `irq_exit`, где
при `!in_interrupt() && local_softirq_pending()` вызывается
`invoke_softirq()`.

Итого, жизненный цикл softirq:

1. **Регистрация** — `open_softirq(nr, action)`;
2. **Активация** — `raise_softirq(nr)` помечает как отложенное;
3. При следующем подходящем случае ядро запускает раунд выполнения;
4. **Выполнение** всех отложенных функций одного типа.

Минус: softirq статичны — модуль не может добавить свой. Это решают
tasklets.

### Tasklets

**Tasklet** — обёртка над softirq (использует `TASKLET_SOFTIRQ` и
`HI_SOFTIRQ`):

- создаются и инициализируются **в рантайме** (годятся для модулей);
- tasklet одного типа **не выполняется на нескольких процессорах
  одновременно**.

Инициализация (`softirq_init`): для каждого CPU обнуляются
per-cpu очереди `tasklet_vec`/`tasklet_hi_vec` (`tasklet_head` со
ссылками `head`/`tail`), затем `open_softirq(TASKLET_SOFTIRQ,
tasklet_action)` и `open_softirq(HI_SOFTIRQ, tasklet_hi_action)`.

Структура tasklet:

```c
struct tasklet_struct
{
        struct tasklet_struct *next;  /* следующий в очереди */
        unsigned long state;          /* состояние */
        atomic_t count;               /* активен или нет */
        void (*func)(unsigned long);  /* колбэк */
        unsigned long data;           /* параметр колбэка */
};
```

API:

```c
/* создание */
void tasklet_init(struct tasklet_struct *t,
                  void (*func)(unsigned long), unsigned long data);
DECLARE_TASKLET(name, func, data);
DECLARE_TASKLET_DISABLED(name, func, data);

/* постановка в очередь */
void tasklet_schedule(struct tasklet_struct *t);            /* обычный */
void tasklet_hi_schedule(struct tasklet_struct *t);         /* высокий */
void tasklet_hi_schedule_first(struct tasklet_struct *t);   /* вне очереди */
```

`tasklet_schedule` через `test_and_set_bit(TASKLET_STATE_SCHED,
&t->state)` (если флаг не стоял) вызывает `__tasklet_schedule` — так
tasklet добавляется в per-cpu очередь.

`__tasklet_schedule` под `local_irq_save` пристыковывает `t` в хвост
per-cpu очереди (`*tail = t; tail = &t->next`) и делает
`raise_softirq_irqoff(TASKLET_SOFTIRQ)`.

Выполнение — в `tasklet_action` (для `HI_SOFTIRQ` — `tasklet_hi_action`):
под `local_irq_disable` вся очередь снимается целиком в локальный
список (голова/хвост per-cpu очереди обнуляются), прерывания
включаются обратно; затем по каждому tasklet — `tasklet_trylock`
(ставит `TASKLET_STATE_RUN`; если уже выполняется — пропуск), вызов
`t->func(t->data)`, `tasklet_unlock`; пустые `next` освобождаются.

### Workqueues

**Workqueue** — отложенная работа в **контексте ядерного потока**, а не
прерывания. Следствия:

- функции workqueue **не обязаны быть атомарными** (в отличие от
  tasklet);
- по умолчанию выполняются на том же процессоре, но это не жёсткое
  правило.

Работа представлена структурой `work_struct`:

```c
struct work_struct {
    atomic_long_t data;      /* параметр / состояние */
    struct list_head entry;  /* элемент списка */
    work_func_t func;        /* функция работы */
#ifdef CONFIG_LOCKDEP
    struct lockdep_map lockdep_map;
#endif
};
```

Выполняют работы per-cpu потоки **`kworker`** (видны в
`systemd-cgls -k` как `[kworker/0:0H]`, `[kworker/1:0H]`, …).

Создание работы: статически `DECLARE_WORK(n, f)` (макрос инициализирует
`work_struct`), в рантайме `INIT_WORK(_work, _func)`.

Постановка в очередь: `queue_work(wq, work)` → `queue_work_on
(WORK_CPU_UNBOUND, wq, work)` (не привязано к конкретному CPU).
`queue_work_on` ставит бит `WORK_STRUCT_PENDING_BIT` и вызывает
`__queue_work`, который определяет целевой **пул воркеров**: для
`req_cpu == WORK_CPU_UNBOUND` берётся `raw_smp_processor_id()`; если
workqueue не `WQ_UNBOUND` — `pwq = per_cpu_ptr(wq->cpu_pwqs, cpu)`,
иначе — `unbound_pwq_by_node(wq, cpu_to_node(cpu))`; далее
`insert_work(pwq, work, worklist, work_flags)`.

Важно: работы кладутся не в саму workqueue, а в **`worker_pool`**
(структура `worker_pool`: `lock`, `cpu`, `node`, `id`, `flags`,
`worklist`, `nr_workers`…). При создании workqueue для каждого CPU
выделяется `pool_workqueue`, связанный со своим `worker_pool`. Поток
`worker_thread` выполняет цикл: снимает `work_struct` со своей очереди
и исполняет.

---

## Практика: serial-драйвер 21285

Пример — serial-драйвер оценочной платы StrongARM SA-110/21285
(`drivers/tty/serial/21285.c`): как драйвер запрашивает IRQ-линию и что
происходит при прерывании.

### Инициализация модуля

```c
module_init(serial21285_init);
module_exit(serial21285_exit);
```

Для загружаемого модуля макросы создают псевдонимы `init_module`
/ `cleanup_module`; для статически вшитого в ядро — `__initcall`
/ `__exitcall`, вызываемые механизмом **initcall**
(`early_initcall`, `core_initcall`, `device_initcall`, `late_initcall`…
— вызываются из `do_initcalls`).

Сама инициализация: `printk(KERN_INFO "Serial: 21285 driver\n")`,
`serial21285_setup_ports()` (задаёт базовую частоту UART:
`uartclk = mem_fclk_21285 / 4`), `uart_register_driver
(&serial21285_reg)` и `uart_add_one_port(...)`. При открытии порта
(`uart_open` → `uart_startup`) вызывается `.startup` из `uart_ops` —
наш `serial21285_startup`.

### Запрос IRQ-линии request_irq

```c
static int serial21285_startup(struct uart_port *port)
{
        int ret;
        tx_enabled(port) = 1;
        rx_enabled(port) = 1;

        ret = request_irq(IRQ_CONRX, serial21285_rx_chars, 0,
                          serial21285_name, port);
        if (ret == 0) {
                ret = request_irq(IRQ_CONTX, serial21285_tx_chars, 0,
                                  serial21285_name, port);
                if (ret)
                        free_irq(IRQ_CONRX, port);
        }
        return ret;
}
```

`RX`/`TX` — пины приёма и передачи последовательной шины. Линии IRQ
платы: `IRQ_CONRX = 16`, `IRQ_CONTX = 17` (ISA-линии 0–15 + смещение).

API `request_irq(irq, handler, flags, name, dev)` — это обёртка над
`request_threaded_irq(irq, handler, NULL, flags, name, dev)`.
Параметры:

- `irq` — запрашиваемый номер прерывания;
- `handler` — указатель на обработчик;
- `flags` — битовая маска опций (`IRQF_*`):
  - `IRQF_SHARED` — общая линия между устройствами;
  - `IRQF_PERCPU` — прерывание привязано к CPU;
  - `IRQF_NO_THREAD` — нельзя превращать в поток;
  - `IRQF_NOBALANCING` — исключить из балансировки;
  - `IRQF_IRQPOLL` — используется для опроса;
  - `0` = `IRQF_TRIGGER_NONE` — без требования edge/level;
- `name` — имя владельца (отображается в `/proc/interrupts`);
- `dev` — указатель для shared-линий (`dev_id`).

### request_threaded_irq и __setup_irq

`request_threaded_irq` (`kernel/irq/manage.c`):

1. Валидация флагов (`IRQF_SHARED` требует `dev_id` и т.п.) — иначе
   `-EINVAL`;
2. `desc = irq_to_desc(irq)`:

   ```c
   struct irq_desc *irq_to_desc(unsigned int irq)
   {
           return (irq < NR_IRQS) ? irq_desc + irq : NULL;
   }
   ```

3. Проверка, что IRQ можно запрашивать (`irq_settings_can_request`);
4. Если `handler == NULL`, а `thread_fn != NULL` — подставляется
   `irq_default_primary_handler` (обработчик будет в потоке);
5. Выделение (`kzalloc`) и заполнение `irqaction`: `handler`,
   `thread_fn`, `flags = irqflags`, `name = devname`,
   `dev_id = dev_id`;
6. Регистрация под замком шины: `chip_bus_lock(desc)`,
   `retval = __setup_irq(irq, desc, action)`,
   `chip_bus_sync_unlock(desc)`; при ошибке `kfree(action)`.

`__setup_irq`: проверки дескриптора/chip, при необходимости создание
потока обработчика: `kthread_create(irq_thread, new, "irq/%d-%s", irq,
new->name)`.

Линии 16 и 17 зарегистрированы — `serial21285_rx_chars` /
`serial21285_tx_chars` будут вызваны при событии.

### Путь прерывания до do_IRQ

Ранее в `native_init_IRQ` были созданы гейты для всех свободных векторов
(`set_intr_gate(i, irq_entries_start + 8 * (i - FIRST_EXTERNAL_VECTOR))`).

Цепочка при срабатывании:

1. Контроллер прерываний уведомляет процессор;
2. Процессор ищет гейт по номеру вектора, кладёт кадр и прыгает в
   `irq_entries_start` (stub `pushq $(~vector+0x80)` + `jmp
   common_interrupt`);
3. `common_interrupt`: нормализация номера и `interrupt do_IRQ` —
   сохранение регистров, `SWAPGS` при необходимости, `irq_count++`;
4. `do_IRQ`:

   ```c
   __visible unsigned int __irq_entry do_IRQ(struct pt_regs *regs)
   {
       struct pt_regs *old_regs = set_irq_regs(regs);
       unsigned vector = ~regs->orig_ax;
       unsigned irq;

       irq_enter();
       exit_idle();
       ...
   }
   ```

   - `irq_enter` — вход в контекст прерывания (обновление
     `__preempt_count`);
   - `exit_idle` — уведомление `idle_notifier`, если процесс был idle
     (pid 0);

5. Определение IRQ и вызов обработчика: `irq = __this_cpu_read
   (vector_irq[vector])`, затем `handle_irq(irq, regs)` →
   `irq_to_desc(irq)` (иначе `return false`) →
   `generic_handle_irq_desc(irq, desc)` → `desc->handle_irq(irq, desc)`.
   `desc->handle_irq` — высокоуровневый обработчик (настраивается при
   инициализации device tree и APIC); он выбирает правильную цепочку и
   вызывает `irq->action(s)` — то есть наши `serial21285_rx_chars` или
   `serial21285_tx_chars`;

6. Завершение: `irq_exit(); set_irq_regs(old_regs); return 1;`

`irq_exit` выходит из контекста прерывания и, при наличии pending-битов,
запускает softirq.

### Выход из прерывания

`do_IRQ` возвращает управление в ассемблер, к метке `ret_from_intr`:
`DISABLE_INTERRUPTS(CLBR_NONE)`, `TRACE_IRQS_OFF`,
`decl PER_CPU_VAR(irq_count)`.

То есть `cli` и декремент `irq_count` (входя, мы его увеличивали).
Далее проверяется прежний контекст (user/kernel), регистры
восстанавливаются соответствующим образом и выполняется выход
`INTERRUPT_RETURN` — это `#define INTERRUPT_RETURN jmp native_iret`,
а `native_iret` заканчивается
инструкцией **`iretq`**, которая восстанавливает `ss:rsp`, `rflags`,
`cs`, `rip` и продолжает прерванное выполнение.

---

## Шпаргалка

### Термины

| Термин | Значение |
| -------- | -------- |
| Interrupt | Событие от hardware/software, требующее CPU |
| Exception | Синхронное исключение выполнения (fault/trap/abort) |
| PIC / APIC | Контроллер прерываний: старый / современный |
| Local APIC | Контроллер внутри каждого ядра CPU |
| I/O APIC | Распределение внешних прерываний между CPU |
| IDT | Таблица адресов обработчиков (гейты) |
| Vector number | Номер записи IDT: 0–255 |
| GDT / TSS | Таблица дескрипторов / сегмент состояния задачи |
| IST | До 7 спецстеков в TSS для критических событий |
| IRQ / ISR | Линия запроса / обработчик внешнего прерывания |
| NMI | Немаскируемое прерывание (вектор 2) |
| SWAPGS | Смена `gs` между user и kernel областью |
| Top half / Bottom half | Быстрая часть / отложенная часть обработки |
| Softirq | Статичный отложенный обработчик (10 типов) |
| Tasklet | Отложенная работа на базе softirq, создается в рантайме |
| Workqueue | Отложенная работа в контексте потока `kworker` |

### Важные векторы x86_64

| Вектор | Исключение |
| -------- | ----------- |
| 0 | `#DE` divide error |
| 1 | `#DB` debug |
| 2 | NMI |
| 3 | `#BP` breakpoint (`int 3`) |
| 6 | `#UD` invalid opcode |
| 8 | `#DF` double fault (IST) |
| 13 | `#GP` general protection |
| 14 | `#PF` page fault |
| 18 | `#MC` machine check (IST) |
| 19 | `#XF` SIMD error |
| 32–127 | Пользовательские (IRQ от 0x20) |
| 0x80 | `ia32_syscall` (при IA32_EMULATION) |
| 0x30–0x3f | Легаси ISA IRQ |

### Функции и макросы

| Вызов | Что делает |
| ------- | ----------- |
| `set_intr_gate(n, addr)` | Гейт с DPL 0 |
| `set_system_intr_gate(n, addr)` | Гейт, доступный из user (DPL 3) |
| `set_intr_gate_ist(n, addr, ist)` | Гейт на IST-стеке |
| `idtentry` | Общая асемблерная обёртка исключения |
| `paranoid_entry/exit` | Вход/выход через медленную проверку MSR |
| `error_entry/error_exit` | Вход/выход для исключений из контекста |
| `local_irq_disable/enable` | `cli`/`sti` на локальном CPU |
| `local_irq_save/restore` | Сохранить `IF`, выключить, восстановить |
| `request_irq` | Зарегистрировать обработчик линии IRQ |
| `free_irq` | Освободить линию IRQ |
| `setup_irq` | Настроить IRQ с готовым `irqaction` |
| `open_softirq` | Зарегистрировать softirq |
| `raise_softirq` | Активировать softirq |
| `tasklet_init/schedule` | Создать / поставить tasklet |
| `DECLARE_WORK/INIT_WORK` | Создать работу workqueue |
| `queue_work` | Поставить работу в очередь |
| `irq_enter/irq_exit` | Вход/выход в контекст прерывания |
| `notify_die` | Рассылка события по notifier chain |

### Цепочка аппаратного прерывания

```text
Устройство → линия IRQ → I/O APIC → Local APIC (вектор)
  → запись в IDT (irq_entries_start)
    → кадр на стеке (SS,RSP,RFLAGS,CS,RIP) [+ error code]
      → SWAPGS (если из user), irq_count++, irq_enter
        → do_IRQ → vector_irq[вектор] → handle_irq
          → desc->handle_irq → irq->action → ISR драйвера
            → irq_exit → (есть pending? → softirq / ksoftirqd)
              → ret_from_intr → iretq → прерванное выполнение
```

### Жизненный цикл отложенной работы

```text
softirq:   open_softirq → raise_softirq → __do_softirq (или ksoftirqd)
tasklet:   tasklet_init → tasklet_schedule → tasklet_action (softirq)
workqueue: INIT_WORK → queue_work → worker_pool → kworker/worker_thread
```

### Полезные файлы ядра

- `arch/x86/include/asm/desc.h` — установка гейтов, `idt_table`;
- `arch/x86/entry/entry_64.S` — `idtentry`, `irq_entries_start`,
  `do_IRQ`-входы, `iret`;
- `arch/x86/kernel/traps.c` — `trap_init`, обработчики исключений;
- `arch/x86/kernel/irqinit.c` — `init_IRQ`, `init_ISA_irqs`;
- `kernel/irq/irqdesc.c` — `early_irq_init`, `irq_to_desc`;
- `kernel/irq/manage.c` — `request_threaded_irq`, `__setup_irq`;
- `kernel/softirq.c` — softirq и tasklets;
- `kernel/workqueue.c` — workqueues, `worker_pool`;
- `drivers/tty/serial/21285.c` — пример драйвера.

Связанные темы: инициализация ядра и ранняя IDT — в конспекте
[вот тут](linux-init-data-structures.md).
