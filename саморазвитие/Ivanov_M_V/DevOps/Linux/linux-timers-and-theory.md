# Таймеры, время и базовая теория ядра Linux

> Конспект по главам *Timers and time management* (части 1–7) и *Theory*
> (paging, ELF, inline assembly) из linux-insides. Полностью переведён
> на русский, вода и повторы вырезаны, сохранены все числа, имена
> структур и порядок вызовов.

---

## 📌 Оглавление

- [Часть I. Таймеры и управление временем](#часть-i-таймеры-и-управление-временем)
  - [Зачем ядру таймеры](#зачем-ядру-таймеры)
  - [jiffies](#jiffies)
  - [Фреймворк clocksource](#фреймворк-clocksource)
  - [Tick broadcast и dyntick](#tick-broadcast-и-dyntick)
  - [Динамические таймеры](#динамические-таймеры)
  - [Фреймворк clockevents](#фреймворк-clockevents)
  - [Clocksource на x86_64](#clocksource-на-x86_64)
  - [Временные системные вызовы](#временные-системные-вызовы)
- [Часть II. Теория ядра](#часть-ii-теория-ядра)
  - [Paging: трансляция адреса](#paging-трансляция-адреса)
  - [ELF: исполняемый и линкуемый формат](#elf-исполняемый-и-линкуемый-формат)
  - [Inline assembly](#inline-assembly)
- [Памятка](#памятка)

---

## Часть I. Таймеры и управление временем

### Зачем ядру таймеры

Управление временем — одна из самых востребованных подсистем ядра. Таймеры
нужны везде: таймауты в реализации TCP, чтение текущего времени, запуск
отложенных функций, планирование следующего события.

Разбирать движок времени будем в том же порядке, в каком ядро его
инициализирует: от `setup_arch` в `start_kernel` до вызовов из
пользовательского пространства.

### jiffies

#### Инициализация

После распаковки ядра и инициализации lockdep, cgroups и stack canary
`start_kernel` вызывает `setup_arch` (`arch/x86/kernel/setup.c`).
Функция готовит архитектурные вещи: резервирует место под `bss` и `initrd`,
парсит командную строку ядра. Там же идёт первое обращение к времени:

```C
x86_init.timers.wallclock_init();
```

Структура `x86_init_ops` (`arch/x86/kernel/x86_init.c`) — набор
указателей на функции для разных платформ (Intel MID, CE4100 и другие).
Поле `timers` типа `x86_init_timers` содержит четыре функции:

| Поле | Назначение |
| --- | --- |
| `setup_percpu_clockev` | per-cpu clock event device для boot-процессора |
| `tsc_pre_init` | вызывается до инициализации TSC |
| `timer_init` | инициализация таймера платформы |
| `wallclock_init` | инициализация часов реального времени |

Для обычного PC `wallclock_init` указывает на заглушку
`x86_init_noop`, то есть функцию с пустым телом. Настоящий RTC нужен
только Intel MID, где `intel_mid_rtc_init` парсит таблицу
`SFI_SIG_MRTC`, делает `set_fixmap_offset_nocache` и подменяет
`x86_platform.get_wallclock` / `set_wallclock`. Обычному x86_64 это не
нужно.

#### Переменная jiffies

Следующий вызов в `setup_arch` — `register_refined_jiffies(CLOCK_TICK_RATE)`.

**Jiffy** — неопределённый короткий промежуток времени. В ядре есть
глобальная переменная `jiffies` — число тиков с момента загрузки:

```C
extern unsigned long volatile __jiffy_data jiffies;
```

Она увеличивается на каждом прерывании системного таймера. Рядом есть
64-битный вариант:

```C
extern u64 jiffies_64;
```

Какая из них используется — зависит от архитектуры, решает линкер-скрипт
`arch/x86/kernel/vmlinux.lds.S`:

```text
#ifdef CONFIG_X86_32
    jiffies = jiffies_64;   /* младшие 32 бита */
#else
    jiffies_64 = jiffies;
#endif
```

Частота прерываний таймера задаётся `HZ` (`include/asm-generic/param.h`),
которая равна `CONFIG_HZ`. Для x86_64 дефолт:

```text
CONFIG_HZ_1000=y
```

Clocksource — абстракция над аппаратным счётчиком: она даёт ядру
значение времени. Нужна потому, что источники времени у разных
устройств разные и имеют разную точность: TSC, HPET, ACPI PM, PIT.
Определение в `kernel/time/jiffies.c`:

```C
static struct clocksource clocksource_jiffies = {
    .name = "jiffies", .rating = 1, .read = jiffies_read,
    .mask = 0xffffffff, .shift = JIFFIES_SHIFT, .max_cycles = 10,
};
```

Ключевые поля:

- `mask` — маска: для 32-битного счётчика `0xffffffff`, то есть
  обнуление каждые ~42 секунды;
- `mult` и `shift` — перевод циклов в наносекунды;
- `max_cycles` — максимум циклов без переполнения.

```C
static cycle_t jiffies_read(struct clocksource *cs)
{
    return (cycle_t) jiffies;
}
```

где `typedef u64 cycle_t;`.

#### Перевод в наносекунды

Формула пересчёта:

```C
((u64) cycles * mult) >> shift;
```

Значения по умолчанию:

```C
#define NSEC_PER_JIFFY  ((NSEC_PER_SEC+HZ/2)/HZ)
#define NSEC_PER_SEC    1000000000L
```

`JIFFIES_SHIFT` зависит от `HZ`:

| Условие | `JIFFIES_SHIFT` |
| --- | --- |
| `HZ < 34` | 6 |
| `HZ < 67` | 7 |
| иначе | 8 |

#### refined_jiffies

Обычный `jiffies` имеет разрешение, равное частоте прерываний, — отсюда
ошибки. `refined_jiffies` считает на частоте PIT (1 193 182 Гц), то есть
даёт разрешение порядка микросекунды.

```C
int register_refined_jiffies(long cycles_per_second)
{
    refined_jiffies = clocksource_jiffies;
    refined_jiffies.name = "refined-jiffies";
    refined_jiffies.rating++;              /* 1 → 2 */
    ...
}
```

Ключевые константы:

```C
#define CLOCK_TICK_RATE  PIT_TICK_RATE
#define PIT_TICK_RATE    1193182ul
```

Расчёт сводится к трём шагам: `cycles_per_tick` из
`(cycles_per_second + HZ/2)/HZ`, затем `shift_hz` и `nsec_per_tick` со
сдвигом влево на 8 бит (это даёт дополнительную точность) и делением
через `do_div`, который округляет частоту вниз во избежание дрейфа.
Результат идёт в `refined_jiffies.mult`, затем вызывается
`__clocksource_register(&refined_jiffies)`.

Чтение безопасно через seqlock — 64-битное значение на некоторых
машинах нельзя прочитать атомарно:

```C
u64 get_jiffies_64(void)
{
    unsigned long seq;
    u64 ret;
    do {
        seq = read_seqbegin(&jiffies_lock);
        ret = jiffies_64;
    } while (read_seqretry(&jiffies_lock, seq));
    return ret;
}
```

Пересчёт в человеческие единицы:

```C
jiffies / HZ                 /* секунды с загрузки (uptime) */
jiffies + 30*HZ              /* через 30 секунд */
jiffies + 120*HZ             /* через 2 минуты */
jiffies + HZ / 1000         /* через 1 миллисекунду */
```

Сравнение времени с учётом переполнения:

```C
#define time_after_eq(a,b)          \
    (typecheck(unsigned long, a) && \
     typecheck(unsigned long, b) && \
    ((long)((a) - (b)) >= 0))
```

Классический таймаут (`arch/x86/kernel/smpboot.c`):

```C
timeout = jiffies + 10*HZ;
while (time_before(jiffies, timeout)) {
    udelay(100);
}
```

### Фреймворк clocksource

#### Зачем нужен

Источники времени в системе разнообразны и дают разное разрешение:
PIT — 1 193 182 Гц, ACPI PM — 3 579 545 Гц, HPET — от 10 МГц,
TSC — частота процессора.

Clocksource — общий API управления этими источниками, независимый от
прерываний. Он решает две задачи: выбрать лучший источник и перевести
его счётчик в наносекунды. Каждый источник обязан вести себя как
монотонно растущий счётчик.

#### Структура clocksource

Определена в `include/linux/clocksource.h` (`____cacheline_aligned`),
ниже перечислены значимые поля:

| `read` | читает текущее значение счётчика |
| `mask` | отсекает старшие биты не-64-битных счётчиков |
| `mult`, `shift` | перевод циклов в наносекунды |
| `max_idle_ns` | максимум простоя для `CONFIG_NO_HZ` |
| `maxadj` | максимальная коррекция `mult` без переполнения |
| `max_cycles` | значение до переполнения |
| `name` | имя источника |
| `list` | список зарегистрированных источников |
| `rating` | приоритет, по нему выбирается лучший |
| `enable` / `disable` | опциональные включение и выключение |
| `flags` | свойства источника |
| `suspend` / `resume` | обработка suspend/resume |
| `archdata` | архитектурные данные (`CONFIG_ARCH_CLOCKSOURCE_DATA`) |
| `wd_list`, `cs_last`, `wd_last` | watchdog (`CONFIG_CLOCKSOURCE_WATCHDOG`) |
| `owner` | модуль-владелец |

`archdata` для x86 и IA64 — это структура с единственным полем
`vclock_mode`: `VCLOCK_NONE`, `VCLOCK_TSC`, `VCLOCK_HPET` или
`VCLOCK_PVCLOCK`.

Watchdog-поля нужны для сверки TSC с другим источником: сам по себе
TSC иногда «врёт», и ядро должно это замечать.

#### Шкала rating

| Диапазон | Значение |
| --- | --- |
| 1–99 | только для загрузки и тестов |
| 100–199 | рабочий, но нежелательный |
| 200–299 | корректный и пригодный |
| 300–399 | быстрый и точный |
| 400–499 | идеальный, обязателен к использованию |

Примеры: TSC — 300, HPET — 250, ACPI PM — 200, refined-jiffies — 2.

#### Регистрация нового источника

Все варианты регистрации — тонкие обёртки над
`__clocksource_register_scale(cs, scale, freq)`, отличаются только
передаваемыми `scale` и `freq`:

| Функция | `scale` | `freq` |
| --- | --- | --- |
| `__clocksource_register` | 1 | 0 |
| `clocksource_register_hz` | 1 | `hz` |
| `clocksource_register_khz` | 1000 | `khz` |

Параметры: `cs` — регистрируемый источник, `scale` — множитель масштаба
(1 = Гц, 1000 = кГц), `freq` — частота, делённая на `scale`. Нулевая
частота означает, что `mult` и `shift` уже заданы вручную.

Основная функция (`kernel/time/clocksource.c`):

```C
int __clocksource_register_scale(struct clocksource *cs,
                                 u32 scale, u32 freq)
{
    __clocksource_update_freq_scale(cs, scale, freq);
    mutex_lock(&clocksource_mutex);
    clocksource_enqueue(cs);
    clocksource_enqueue_watchdog(cs);
    clocksource_select();
    mutex_unlock(&clocksource_mutex);
    return 0;
}
```

`clocksource_mutex` защищает `curr_clocksource` (текущий источник) и
`clocksource_list` (список зарегистрированных).

#### Расчёт mult и shift

Если частота передана, ядро считает `mult` и `shift` само
(`__clocksource_update_freq_scale`): сначала вычисляется `sec` — через
сколько секунд счётчик переполнится (`cs->mask`, делённый на `freq` и
`scale`), значение зажимается в диапазон 1…600 секунд, затем вызывается
`clocks_calc_mult_shift(&cs->mult, &cs->shift, freq, NSEC_PER_SEC / scale,
sec * scale)`.

В конце ядро проверяет переполнение `mult`, обновляет `max_idle_ns` и
`max_cycles` и печатает результат в `dmesg`.

#### Вставка в список и выбор

```C
static void clocksource_enqueue(struct clocksource *cs)
{
    struct list_head *entry = &clocksource_list;
    struct clocksource *tmp;

    list_for_each_entry(tmp, &clocksource_list, list)
        if (tmp->rating >= cs->rating)
            entry = &tmp->list;
    list_add(&cs->list, entry);
}
```

Список отсортирован по rating, лучший всегда первый. Выбор:

```C
static void clocksource_select(void)
{
    return __clocksource_select(false);
}
```

Внутри, если найден лучший источник и текущий отличается:

```C
if (curr_clocksource != best && !timekeeping_notify(best)) {
    pr_info("Switched to clocksource %s\n", best->name);
    curr_clocksource = best;
}
```

#### Интерфейс sysfs

`kernel/time/clocksource.c` на `device_initcall` регистрирует подсистему
`clocksource`, устройство `device_clocksource` и три файла атрибутов:
`current_clocksource` (текущий источник), `available_clocksource`
(список доступных) и `unbind_clocksource` (отвязка). Проверка:

```bash
$ cat /sys/devices/system/clocksource/clocksource0/current_clocksource
tsc
$ cat /sys/devices/system/clocksource/clocksource0/available_clocksource
tsc hpet acpi_pm
```

---

### Tick broadcast и dyntick

#### Idle-задача и природа прерываний

Когда процессору нечего выполнять, ядро запускает idle-задачу. После
инициализации `rest_init` вызывает `cpu_idle_loop`
(`kernel/sched/idle.c`) — бесконечный цикл:

```C
static void cpu_idle_loop(void)
{
    while (1) {
        while (!need_resched()) {
            cpuidle_idle_call();
        }
        schedule_preempt_disabled();
    }
}
```

Процессор крутится в бесконечном цикле, но его прерывает системный
таймер: после обработки прерывания `need_resched` становится истинным,
и управление уходит на runnable-задачу.

Проблема: будить процессор прерыванием, когда он ничего не делает,
расточительно с точки зрения энергопотребления. Отсюда два механизма
экономии.

#### Два режима без регулярных тиков

| Опция | Поведение |
| --- | --- |
| `CONFIG_NO_HZ_IDLE` | Тики пропускаются на idle-процессорах (dyntick-idle) |
| `CONFIG_NO_HZ_FULL` | Тики пропускаются, если задача одна |

В обоих случаях периодические прерывания заменяются событиями по
требованию: планировщик сам выставляет таймер на момент следующей задачи.
Вход и выход из режима — функции `tick_nohz_idle_enter` и
`tick_nohz_idle_exit` (`kernel/time/tick-sched.c`).

Отдельная проблема: локальный таймер процессора останавливается в
C-состояниях. Чтобы разбудить уснувший процессор, нужен таймер, не
зависящий от C-state. Это и есть **tick broadcast**.

`tick_init` (`kernel/time/tick-common.c`) делает две вещи:
`tick_broadcast_init()` и `tick_nohz_init()`.

#### tick_broadcast_init

Функция выделяет шесть cpumask через `zalloc_cpumask_var` (это
`alloc_cpumask_var` с флагом `__GFP_ZERO`, в конце концов
`kmalloc_node(cpumask_size(), flags, node)`):

| Cpumask | Назначение |
| --- | --- |
| `tick_broadcast_mask` | процессоры, которые сейчас спят |
| `tick_broadcast_on` | процессоры в периодическом broadcast |
| `tmpmask` | временный набор для вычислений |
| `tick_broadcast_oneshot_mask` | кто должен быть уведомлён (oneshot) |
| `tick_broadcast_pending_mask` | кто ожидает broadcast |
| `tick_broadcast_force_mask` | принудительный broadcast |

Три последних выделяются только при `CONFIG_TICK_ONESHOT`. Сами режимы
clock event device задаются флагами `CLOCK_EVT_FEAT_PERIODIC` (0x1) и
`CLOCK_EVT_FEAT_ONESHOT` (0x2) в `include/linux/clockchips.h`.

#### Установка broadcast-устройства

Каждый процессор имеет свой `tick_device` (`kernel/time/tick-sched.h`):

```C
struct tick_device {
    struct clock_event_device *evtdev;
    enum tick_device_mode mode;
};
```

Устройство регистрируется через `clockevents_register_device`, после
чего `tick_check_new_device` вызывает `tick_install_broadcast_device`.
Порядок его работы:

1. `tick_check_broadcast_device` — проверка пригодности (по флагам
   `features` и `rating` против текущего).
2. `try_module_get(dev->owner)` — захват модуля-владельца.
3. `clockevents_exchange_device` — обмен устройствами, старому
   ставится заглушка `clockevents_handle_noop`.
4. `tick_broadcast_start_periodic(dev)`, если `tick_broadcast_mask`
   не пуст (кто-то уже спит).
5. `tick_clock_notify()` для oneshot-устройств.

Процессоры, чей локальный таймер не работает, попадают в
`tick_broadcast_mask` в функции `tick_device_uses_broadcast`.

#### Путь прерывания

`tick_broadcast_start_periodic` зовёт `tick_setup_periodic(bc, 1)`, а
`tick_set_periodic_handler` выбирает обработчик по признаку broadcast:
`tick_handle_periodic` или `tick_handle_periodic_broadcast`.

Обработчик прерывания HPET (`arch/x86/kernel/hpet.c`) проверяет, что
обработчик установлен (иначе это ложное прерывание), и вызывает его:

```C
    if (!hevt->event_handler) {
        printk(KERN_INFO "Spurious HPET timer interrupt on HPET "
               "timer %d\n", dev->num);
        return IRQ_HANDLED;
    }
    hevt->event_handler(hevt);
```

Дальше `tick_handle_periodic_broadcast` собирает спящие процессоры и
отправляет им IPI:

```text
tick_handle_periodic_broadcast
  └─ tick_do_periodic_broadcast
       cpumask_and(tmpmask, cpu_online_mask, tick_broadcast_mask)
       └─ tick_do_broadcast(tmpmask)   → IPI
            └─ td->evtdev->event_handler(td->evtdev)
                 → локальный обработчик таймера → CPU просыпается
```

#### tick_nohz_init

Инициализация структур dyntick (`kernel/time/tick-sched.c`):

1. Если `tick_nohz_full_running` не выставлен, `tick_nohz_init_all`
   выделяет `tick_nohz_full_mask` (`CONFIG_NO_HZ_FULL_ALL`), ставит
   все биты и выставляет `tick_nohz_full_running = true`.
2. Выделяется `housekeeping_mask` — процессоры, которые **не** уходят в
   NO_HZ.
3. Проверяется `arch_irq_work_has_interrupt()`. Для x86_64 это просто
   `cpu_has_apic`: без APIC нельзя разбудить процессор IPI, поэтому
   маска очищается и выводится предупреждение.
4. Текущий процессор убирается из `tick_nohz_full_mask` — он будет
   заниматься timekeeping.
5. `housekeeping_mask` наполняется как `cpu_possible_mask` минус
   `tick_nohz_full_mask`.
6. Для каждого процессора из `tick_nohz_full_mask` вызывается
   `context_tracking_cpu_set(cpu)`, которая ставит `context_tracking.active`
   в `true` — после этого context tracking игнорирует переключения
   контекста на этом процессоре.

Ключевой момент: **хотя бы один процессор всегда остаётся в обычном
режиме** и занимается учётом времени.

---

### Динамические таймеры

#### Что это

Таймеры в ядре — способ вызвать функцию в будущем. Пример из
`net/netfilter/ipset/ip_set_list_set.c`, где сборщик мусора работает по
таймеру: в `struct list_set` лежит поле `gc` типа `timer_list`, а
инициализация задаёт функцию и срок:

```C
map->gc.function = gc;
map->gc.expires = jiffies + IPSET_GC_PERIOD(set->timeout) * HZ;
```

У ядра два вида таймеров: **динамические** (нужны ядру, живут в
`timer_list`) и **интервальные** (доступны из userspace).

#### init_timers

```C
void __init init_timers(void)
{
    init_timer_cpus();             /* per-cpu tvec_base */
    init_timer_stats();            /* lock для статистики */
    timer_register_cpu_notifier(); /* миграция при CPU_DEAD */
    open_softirq(TIMER_SOFTIRQ, run_timer_softirq);
}
```

#### Структура tvec_base

`init_timer_cpus` вызывает `init_timer_cpu` для каждого **possible**
процессора (того, который можно подключить в любой момент):

```C
struct tvec_base {
    spinlock_t lock;
    /* ... поля, перечисленные в таблице ниже ... */
} ____cacheline_aligned;
```

| Поле | Назначение |
| --- | --- |
| `lock` | spinlock на структуру |
| `running_timer` | выполняющийся сейчас таймер |
| `timer_jiffies` | ближайшее время истечения |
| `next_timer` | следующий таймер для `NO_HZ` |
| `active_timers` | таймеры, не останавливаемые при засыпании |
| `all_timers` | всего таймеров |
| `cpu` | номер процессора-владельца |
| `migration_enabled` | разрешена ли миграция таймеров |
| `nohz_active` | статус режима `NO_HZ` |
| `tv1`…`tv5` | списки таймеров, см. ниже |

Инициализация per-cpu переменной:

```C
static DEFINE_PER_CPU(struct tvec_base, tvec_bases);

static void __init init_timer_cpu(int cpu)
{
    struct tvec_base *base = per_cpu_ptr(&tvec_bases, cpu);
    base->cpu = cpu;
    spin_lock_init(&base->lock);
    base->timer_jiffies = jiffies;
    base->next_timer = base->timer_jiffies;
}
```

#### Каскад списков tv1–tv5

Таймеры хранятся не в одном списке, а в каскаде из пяти уровней —
это позволяет за O(1) находить ближайший срок без перебора всех
таймеров.

| Список | Временной диапазон |
| --- | --- |
| `tv1` | до 255 тиков |
| `tv2` | до 2^14 − 1 тиков |
| `tv3` | до 2^20 − 1 тиков |
| `tv4` | до 2^26 тиков |
| `tv5` | большие интервалы |

Размер `tv1` задаётся `#define TVR_BITS (CONFIG_BASE_SMALL ? 6 : 8)`,
то есть 64 или 256 элементов. `CONFIG_BASE_SMALL` уменьшает структуры
данных ядра ради экономии памяти.

#### Статистика и hotplug

`init_timer_stats` инициализирует per-cpu raw spinlock
`tstats_lookup_lock`, которым защищается статистика. Она видна в
`/proc/timer_stats`:

```bash
$ cat /proc/timer_stats
Timerstats sample period: 3.888770 s
  12,     0 swapper          hrtimer_stop_sched_tick
  15,     1 swapper          hcd_submit_urb
   4,   959 kedac            schedule_timeout
```

При логическом выключении процессора приходит `CPU_DEAD` или
`CPU_DEAD_FROZEN`, и `timer_cpu_notify` вызывает `migrate_timers`, чтобы
перенести его таймеры на другой процессор. Всё это под
`CONFIG_HOTPLUG_CPU`.

#### Обработка таймеров через softirq

Таймеры **не** обрабатываются прямо в обработчике аппаратного
прерывания: это слишком долго. Вместо этого поднимается отложенное
прерывание:

```C
open_softirq(TIMER_SOFTIRQ, run_timer_softirq);
```

`run_timer_softirq` выполняется в конце `do_IRQ`
(`arch/x86/kernel/irq.c`), берёт `tvec_bases` текущего процессора и
сравнивает текущее время с `timer_jiffies`; если срок наступил — зовёт
`__run_timers(base)`.

#### __run_timers

`__run_timers` берёт `spin_lock_irq` на `base` и крутит цикл, пока
`time_after_eq(jiffies, base->timer_jiffies)`:

```C
index = base->timer_jiffies & TVR_MASK;

if (!index &&
    (!cascade(base, &base->tv2, INDEX(0))) &&
        (!cascade(base, &base->tv3, INDEX(1))) &&
                !cascade(base, &base->tv4, INDEX(2)))
        cascade(base, &base->tv5, INDEX(3));

++base->timer_jiffies;
```

Дальше список `base->tv1.vec + index` переносится во временный `head` и
перебирается: для каждого таймера запоминаются `fn` и `data`, после
чего **lock снимается**, вызывается `call_timer_fn(timer, fn, data)`,
и lock берётся обратно.

Снятие блокировки здесь принципиально: обработчик таймера — это
пользовательский код, он может вызвать таймерные функции, и удержание
lock привело бы к deadlock.

#### API для разработчика

| Операция | Вызов |
| --- | --- |
| Инициализация | `init_timer(timer)` |
| Инициализация с данными | `TIMER_INITIALIZER(fn, expires, data)` |
| Запуск | `add_timer(timer)` |
| Остановка | `del_timer(timer)` |

---

### Фреймворк clockevents

#### Обратная сторона clocksource

Если clocksource отвечает на вопрос «сколько сейчас времени»
(предоставляет timeline), то clockevents — на вопрос «когда позвать»
(событие в будущем). Документация формулирует это прямо: *clock events
are the conceptual reverse of clock sources*.

#### Структура clock_event_device

```C
#include/linux/clockchips.h
```

Ключевые поля: `name` (например `"lapic"`), `event_handler` (обработчик
прерывания), `set_next_event` (программирование следующего события),
`features`, а также `mult` / `shift` / `rating` / `cpumask`.

Флаги возможностей:

```C
#define CLOCK_EVT_FEAT_PERIODIC 0x000001
#define CLOCK_EVT_FEAT_ONESHOT  0x000002
#define CLOCK_EVT_FEAT_C3STOP   0x000008
```

`CLOCK_EVT_FEAT_C3STOP` — устройство останавливается в C3-состоянии,
то есть не разбудит процессор из глубокого сна.

#### Состояния устройства

```C
enum clock_event_state {
    CLOCK_EVT_STATE_DETACHED,
    CLOCK_EVT_STATE_SHUTDOWN,
    CLOCK_EVT_STATE_PERIODIC,
    CLOCK_EVT_STATE_ONESHOT,
    CLOCK_EVT_STATE_ONESHOT_STOPPED,
};
```

| Состояние | Значение |
| --- | --- |
| `DETACHED` | устройство не используется (начальное) |
| `SHUTDOWN` | выключено |
| `PERIODIC` | генерирует события периодически |
| `ONESHOT` | генерирует одно событие |
| `ONESHOT_STOPPED` | oneshot временно остановлен |

#### Пример регистрации устройства

PIT для `at91sam926x` (`drivers/clocksource/timer-atmel-pit.c`):

```C
static void __init at91sam926x_pit_common_init(struct pit_data *data)
{
    data->clkevt.name = "pit";
    data->clkevt.features = CLOCK_EVT_FEAT_PERIODIC;
    data->clkevt.shift = 32;
    data->clkevt.mult = div_sc(pit_rate, NSEC_PER_SEC,
                               data->clkevt.shift);
    data->clkevt.rating = 100;
    data->clkevt.cpumask = cpumask_of(0);
    data->clkevt.set_state_shutdown = pit_clkevt_shutdown;
    data->clkevt.set_state_periodic = pit_clkevt_set_periodic;
    data->clkevt.resume = at91sam926x_pit_resume;
    data->clkevt.suspend = at91sam926x_pit_suspend;
}
```

`cpumask_of(0)` — маска с одним процессором, для которого работает это
устройство; `rating = 100` означает, что источник рабочий, но не
приоритетный.

#### clockevents_register_device

Сначала устройство переводится в `CLOCK_EVT_STATE_DETACHED`, а пустой
`cpumask` заменяется на маску текущего процессора (с предупреждением,
если процессоров больше одного). Затем под спинлоком с запретом
локальных прерываний выполняются четыре строки:

```C
raw_spin_lock_irqsave(&clockevents_lock, flags);
list_add(&dev->list, &clockevent_devices);
tick_check_new_device(dev);
clockevents_notify_released();
raw_spin_unlock_irqrestore(&clockevents_lock, flags);
```

Запрет прерываний обязателен: прерывание от другого clock event device
во время добавления в список дало бы deadlock.

#### Выбор устройства

`tick_check_new_device` сравнивает новое устройство с текущим.
Предпочтение отдаётся oneshot:

```C
static bool tick_check_preferred(struct clock_event_device *curdev,
                 struct clock_event_device *newdev)
{
    if (!(newdev->features & CLOCK_EVT_FEAT_ONESHOT)) {
        if (curdev && (curdev->features & CLOCK_EVT_FEAT_ONESHOT))
            return false;
        if (tick_oneshot_mode_active())
            return false;
    }
    return !curdev ||
        newdev->rating > curdev->rating ||
           !cpumask_equal(curdev->cpumask, newdev->cpumask);
}
```

Если устройство предпочтительнее, происходит обмен и установка:

```C
clockevents_exchange_device(curdev, newdev);
tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
```

`tick_setup_device` смотрит на режим:

```C
if (td->mode == TICKDEV_MODE_PERIODIC)
    tick_setup_periodic(newdev, 0);
else
    tick_setup_oneshot(newdev, handler, next_event);
```

#### Смена состояния

`__clockevents_switch_state` для `SHUTDOWN` и `DETACHED` зовёт
`set_state_shutdown`, для `PERIODIC` — `set_state_periodic` (и
возвращает `-ENOSYS`, если устройство не поддерживает периодический
режим). Реальная работа происходит в коллбэке устройства: для PIT
`at91sam926x` это запись в регистр режима `AT91_PIT_MR`, где биты 24
и 25 — `PITEN` и `PITIEN`.

Устройство остаётся снятым с учёта, пока не будет вызвана
`clockevents_notify_released`, которая возвращает их в
`clockevent_devices` и заново прогоняет `tick_check_new_device`.

---

### Clocksource на x86_64

#### Что доступно в системе

```bash
$ cat /sys/devices/system/clocksource/clocksource0/available_clocksource
tsc hpet acpi_pm
$ cat /sys/devices/system/clocksource/clocksource0/current_clocksource
tsc
```

`jiffies` и `refined_jiffies` в список не попадают: файл показывает
только источники с флагом `CLOCK_SOURCE_VALID_FOR_HRES`, то есть с
высоким разрешением.

| Источник | Частота | Rating |
| --- | --- | --- |
| `acpi_pm` | 3,579545 МГц | 200 |
| `hpet` | от 10 МГц | 250 |
| `tsc` | частота CPU, на новых CPU инвариантный | 300 |

На старых процессорах TSC считал внутренние такты и менял частоту
вместе с процессором. На новых есть **invariant TSC** — счётчик с
постоянной частотой во всех рабочих состояниях. Частоту можно посмотреть
в `/proc/cpuinfo` по model name.

#### Поздняя инициализация

`start_kernel` после `setup_arch` вызывает `late_time_init()`, который
для x86 (`arch/x86/kernel/time.c`) делает две вещи:

```C
static __init void x86_late_time_init(void)
{
    x86_init.timers.timer_init();  /* → hpet_time_init */
    tsc_init();
}
```

#### HPET

`arch/x86/kernel/hpet.c`:

```C
void __init hpet_time_init(void)
{
    if (!hpet_enable())
        setup_pit_timer();
    setup_default_timer_irq();
}
```

`hpet_enable` проверяет `is_hpet_capable()` и маппит регистры
(`HPET_MMAP_SIZE` = 1024 байт):

```C
hpet_virt_address = ioremap_nocache(hpet_address, HPET_MMAP_SIZE);
```

`is_hpet_capable` проверяет, что в командной строке нет `hpet=disable`
и что адрес получен из таблицы ACPI HPET.

Дальше читается `HPET_ID` для числа таймеров, выделяется память под
`General Configuration Register`, разрешается счётчик и регистрируется
clocksource:

```C
static struct clocksource clocksource_hpet = {
    .name     = "hpet",
    .rating   = 250,
    .read     = read_hpet,
    .mask     = HPET_MASK,
    .flags    = CLOCK_SOURCE_IS_CONTINUOUS,
    .resume   = hpet_resume_counter,
    .archdata = { .vclock_mode = VCLOCK_HPET },
};

clocksource_register_hz(&clocksource_hpet, (u32)hpet_freq);
```

Чтение счётчика — одна строка:
`return (cycle_t)hpet_readl(HPET_COUNTER);`. Затем
`setup_default_timer_irq` настраивает IRQ0 с учётом наличия legacy
i8259.

#### ACPI PM timer

`drivers/clocksource/acpi_pm.c`, регистрация на `fs_initcall`:

```C
static int __init init_acpi_pm_clocksource(void)
{
    if (!pmtmr_ioport)
        return -ENODEV;
    ...
    clocksource_register_hz(&clocksource_acpi_pm, PMTMR_TICKS_PER_SEC);
}
```

Адрес регистра берётся из таблицы FADT
(`arch/x86/kernel/acpi/boot.c`) в `acpi_parse_fadt`:
`pmtmr_ioport = acpi_gbl_FADT.xpm_timer_block.address;`

Сам регистр 32-битный, но верхние 8 бит заняты признаком ошибки
`E_TMR_VAL`, поэтому берутся только 24 младших бита:

```C
static inline u32 read_pmtmr(void)
{
    return inl(pmtmr_ioport) & ACPI_PM_MASK;
}

static struct clocksource clocksource_acpi_pm = {
    .name   = "acpi_pm",
    .rating = 200,
    .read   = acpi_pm_read,
    .mask   = (cycle_t)ACPI_PM_MASK,
    .flags  = CLOCK_SOURCE_IS_CONTINUOUS,
};
```

#### Time Stamp Counter

`arch/x86/kernel/tsc.c`:

`tsc_init` первым делом проверяет `cpu_has_tsc` — это
`boot_cpu_has(X86_FEATURE_TSC)`, то есть проверка бита в
`boot_cpu_data`. Если поддержки нет, снимается
`X86_FEATURE_TSC_DEADLINE_TIMER` и функция выходит.

Дальше измеряется частота и заполняется масштаб для всех процессоров:

```C
tsc_khz = x86_platform.calibrate_tsc();
cpu_khz = tsc_khz;

for_each_possible_cpu(cpu) {
    cyc2ns_init(cpu);
    set_cyc2ns_scale(cpu_khz, cpu);
}
```

Затем проверяется, не отключён ли TSC, и вызывается
`check_system_tsc_reliable()`. Сам clocksource регистрируется позже, на
`device_initcall` — чтобы TSC оказался после HPET:

```C
static int __init init_tsc_clocksource(void)
{
    if (!cpu_has_tsc || tsc_disabled > 0 || !tsc_khz)
        return 0;
    if (boot_cpu_has(X86_FEATURE_TSC_RELIABLE)) {
        clocksource_register_khz(&clocksource_tsc, tsc_khz);
        return 0;
    }
}
```

```C
static struct clocksource clocksource_tsc = {
    .name     = "tsc",
    .rating   = 300,
    .read     = read_tsc,
    .mask     = CLOCKSOURCE_MASK(64),
    .flags    = CLOCK_SOURCE_IS_CONTINUOUS |
                CLOCK_SOURCE_MUST_VERIFY,
    .archdata = { .vclock_mode = VCLOCK_TSC },
};
```

#### Порядок в dmesg

```text
[    0.000000] clocksource: refined-jiffies: mask: 0xffffffff ...
[    0.000000] clocksource: hpet: mask: 0xffffffff ...
[    0.094369] clocksource: jiffies: mask: 0xffffffff ...
[    0.186498] clocksource: Switched to clocksource hpet
[    0.196827] clocksource: acpi_pm: mask: 0xffffff ...
[    1.413685] tsc: Refined TSC clocksource calibration: 3999.981 MHz
[    1.413688] clocksource: tsc: mask: 0xffffffffffffffff ...
[    2.413748] clocksource: Switched to clocksource tsc
```

---

### Временные системные вызовы

#### gettimeofday

```C
struct timeval {
    time_t      tv_sec;     /* секунды */
    suseconds_t tv_usec;    /* микросекунды */
};

gettimeofday(&time, NULL);
```

`gettimeofday` **не** обычный syscall: он живёт в vDSO. glibc сначала
пытается найти символ, и только при неудаче откатывается на настоящий
syscall:

```C
return (_dl_vdso_vsym("__vdso_gettimeofday", &linux26)
  ?: (void*) (&__gettimeofday_syscall));
```

Точка входа — weak alias:

```C
int gettimeofday(struct timeval *, struct timezone *)
    __attribute__((weak, alias("__vdso_gettimeofday")));
```

`__vdso_gettimeofday` вызывает `do_realtime`; при `VCLOCK_NONE` делается
прямой `syscall` через `vdso_fallback_gtod`:

```C
notrace static long vdso_fallback_gtod(struct timeval *tv,
                                       struct timezone *tz)
{
    long ret;
    asm("syscall" : "=a" (ret) :
        "0" (__NR_gettimeofday), "D" (tv), "S" (tz) : "memory");
    return ret;
}
```

`do_realtime` читает структуру `vsyscall_gtod_data`
(`arch/x86/include/asm/vgtod.h`), обновляемую по прерыванию таймера:

```C
do {
    seq = gtod_read_begin(gtod);
    mode = gtod->vclock_mode;
    ts->tv_sec = gtod->wall_time_sec;
    ns = gtod->wall_time_snsec;
    ns += vgetsns(&mode);
    ns >>= gtod->shift;
} while (unlikely(gtod_read_retry(gtod, seq)));

ts->tv_sec += __iter_div_u64_rem(ns, NSEC_PER_SEC, &ns);
ts->tv_nsec = ns;
```

`vgetsns` догоняет текущий счётчик выбранного clocksource, поэтому ответ
точнее, чем простое чтение `wall_time_sec`.

#### clock_gettime

```C
clock_gettime(CLOCK_BOOTTIME, &elapsed_from_boot);
```

Возвращает время, заданное идентификатором часов:

| `clock_id` | Значение |
| --- | --- |
| `CLOCK_REALTIME` | системные часы, wall-clock |
| `CLOCK_REALTIME_COARSE` | быстрая версия `CLOCK_REALTIME` |
| `CLOCK_MONOTONIC` | монотонное от неопределённой точки |
| `CLOCK_MONOTONIC_COARSE` | быстрая версия `CLOCK_MONOTONIC` |
| `CLOCK_MONOTONIC_RAW` | как `MONOTONIC`, но без поправки NTP |
| `CLOCK_BOOTTIME` | `MONOTONIC` плюс время сна системы |
| `CLOCK_PROCESS_CPUTIME_ID` | CPU-время процесса, все потоки |
| `CLOCK_THREAD_CPUTIME_ID` | CPU-время одного потока |

Точка входа — та же `arch/x86/entry/vdso/vclock_gettime.c`:

```C
notrace int __vdso_clock_gettime(clockid_t clock, struct timespec *ts)
{
    switch (clock) {
    case CLOCK_REALTIME:
        if (do_realtime(ts) == VCLOCK_NONE)
            goto fallback;
        break;
    ...
    }
fallback:
    return vdso_fallback_gettime(clock, ts);
}
```

`do_monotonic` отличается только источником полей: `gtod->monotonic_time_sec`
и `gtod->monotonic_time_snsec` вместо `wall_time_sec` / `wall_time_snsec`.

#### nanosleep

```C
int nanosleep(const struct timespec *req, struct timespec *rem);
```

`req` — на сколько спать, `rem` — сколько осталось, если вызов
прервали сигналом. Этого syscall **нет** в vDSO, glibc собирает
аргументы макросом `INTERNAL_SYSCALL` и выполняет `syscall` напрямую:

```C
# define INTERNAL_SYSCALL(name, err, nr, args...) \
  INTERNAL_SYSCALL_NCS (__NR_##name, err, nr, ##args)

```

`LOAD_ARGS_##nr` и `LOAD_REGS_##nr` раскладывают аргументы по
регистрам (`rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`), после чего идёт
`syscall`, а clobber `memory` запрещает оптимизацию.

Обработчик — `kernel/time/hrtimer.c`:

```C
SYSCALL_DEFINE2(nanosleep, struct timespec __user *, rqtp,
        struct timespec __user *, rmtp)
{
    struct timespec tu;

    if (copy_from_user(&tu, rqtp, sizeof(tu)))
        return -EFAULT;

    if (!timespec_valid(&tu))
        return -EINVAL;

    return hrtimer_nanosleep(&tu, rmtp, HRTIMER_MODE_REL,
                             CLOCK_MONOTONIC);
}
```

Валидация:

```C
static inline bool timespec_valid(const struct timespec *ts)
{
    if (ts->tv_sec < 0)
        return false;
    if ((unsigned long)ts->tv_nsec >= NSEC_PER_SEC)
        return false;
    return true;
}
```

`hrtimer_nanosleep` создаёт высокоточный таймер и уходит в `do_nanosleep`:

```C
do {
    set_current_state(TASK_INTERRUPTIBLE);
    hrtimer_start_expires(&t->timer, mode);
    if (likely(t->task))
        freezable_schedule();
} while (t->task && !signal_pending(current));

__set_current_state(TASK_RUNNING);
return t->task == NULL;
```

`TASK_INTERRUPTIBLE` — задача спит, но просыпается по сигналу. Когда
таймер истекает, задача снова получает управление.

---

## Часть II. Теория ядра

### Paging: трансляция адреса

#### Зачем это знать

Управление памятью — самая сложная часть ядра. Прежде чем разбирать
инициализацию, нужно понять механизм, без которого ядро не запустится:
**paging** — механизм трансляции линейного адреса в физический.

В реальном режи адрес считался сдвигом сегментного регистра на 4 плюс
смещение. В защищённом режиме добавились таблицы дескрипторов с базовыми
адресами. Теперь — 64-битный режим и страничная трансляция.

> Paging provides a mechanism for implementing a conventional
> demand-paged, virtual-memory system where sections of a program's
> execution environment are mapped into physical memory as needed.

#### Три режима и включение

| Режим | Описание |
| --- | --- |
| 32-bit paging | устаревший, 2 уровня |
| PAE paging | 3 уровня, для Pentium+ |
| IA-32e paging | 4 уровня, актуален для x86_64 |

Для включения IA-32e paging нужны три бита:

```assembly
movl $(X86_CR0_PG | X86_CR0_PE), %eax
movl %eax, %cr0
```

```assembly
movl $MSR_EFER, %ecx
rdmsr
btsl $_EFER_LME, %eax
wrmsr
```

То есть `CR0.PG`, `CR0.PE` и `IA32_EFER.LME`.

#### Страничные структуры

Линейное пространство делится на страницы по **4096 байт**. Каждая
структура трансляции тоже 4096 байт и содержит **512 записей**.

Linux на x86_64 использует 4 уровня. CPU берёт часть линейного адреса,
чтобы найти запись в структуре следующего уровня, и так до тех пор, пока
не получит физический адрес. Адрес верхнего уровня лежит в `cr3`:

```assembly
leal pgtable(%ebx), %eax
movl %eax, %cr3
```

`cr3` в Linux называется **PML4** (Page Global Directory). Он 64-битный:
биты 63:52 и 11:5 зарезервированы, 51:12 хранят адрес структуры
верхнего уровня, 4:3 — `PWT` (write-through) и `PCD` (cache disable),
2:0 игнорируются.

#### Разбор линейного адреса

Линейный адрес приходит не в память, а в MMU. Значимы только младшие
48 бит — то есть одновременно доступно `2^48` байт (256 ТБ).

| Биты | Куда идут |
| --- | --- |
| 47:39 | индекс в структуре 4-го уровня |
| 38:30 | индекс в структуре 3-го уровня |
| 29:21 | индекс в структуре 2-го уровня |
| 20:12 | индекс в структуре 1-го уровня |
| 11:0 | смещение внутри физической страницы |

Схема 4-уровневой трансляции:

```text
Линейный адрес
+------------+------------+------------+------------+------------+
| 47:39      | 38:30      | 29:21      | 20:12      | 11:0       |
+------------+------------+------------+------------+------------+
     v             v             v             v
  PML4          PDPT           PD            PT     → физ. адрес
 (ур.4)        (ур.3)        (ур.2)        (ур.1)
```

#### Биты записи страничной таблицы

| Бит | Имя | Значение |
| --- | --- | --- |
| 63 | `N/X` | запрет выполнения кода со страницы |
| 62:52 | — | игнорируются CPU, для ОС |
| 51:12 | — | физический адрес структуры ниже |
| 11:9 | — | игнорируются CPU |
| — | `A` | страница была доступна |
| 6 | `D` | страница была изменена |
| 5 | `PWT` | политика кэша (write-through) |
| 4 | `PCD` | запрет кэширования |
| 2 | `U/S` | доступ из user space (0 — только ядро) |
| 1 | `R/W` | разрешена запись (0 — только чтение) |
| 0 | `P` | страница присутствует в памяти |

Бит `P` — самый важный для понимания: если он сброшен, обращение к
адресу вызывает page fault, именно так работает ленивое выделение
памяти.

#### Виртуальное пространство ядра

Адресное пространство x86_64 формально `2^64`, но используются только
48 бит. Проблема решается **sign extension**: младшие 48 бит адреса
хранят собственно адрес, а биты 63:48 либо все нули, либо все единицы.

```text
0xffffffffffffffff  +-----------+
                    |           |
                    |  Kernelspace
                    |           |
0xffff800000000000  +-----------+
                    |           |
                    |    hole
                    |           |
                    |           |
0x00007fffffffffff  +-----------+
                    |           |
                    |  Userspace
                    |           |
0x0000000000000000  +-----------+
```

| Начало | Конец | Размер | Назначение |
| --- | --- | --- | --- |
| `0000000000000000` | `00007fffffffffff` | 47 бит | userspace |
| — | — | — | дыра от sign extension |
| `ffff800000000000` | `ffff87ffffffffff` | — | guard hole, резерв гипервизора |
| `ffff880000000000` | `ffffc7ffffffffff` | 64 ТБ | прямая карта RAM |
| `ffffc90000000000` | `ffffe8ffffffffff` | 45 бит | vmalloc и ioremap |
| `ffffec0000000000` | `ffffffff00000000` | 16 ТБ | shadow memory KASAN |
| `ffffffff80000000` | `ffffffffa0000000` | 512 МБ | kernel text mapping |
| `ffffffffa0000000` | `fffffffff5ffffff` | 1525 МБ | пространство модулей |
| `fffffffff600000` | `fffffffffdffffff` | 8 МБ | vsyscalls |

Адреса с корректным sign extension называются **canonical**.

Ключевые константы в `arch/x86/include/asm/page_64_types.h`:

```C
#define __PAGE_OFFSET _AC(0xffff880000000000, UL)
#define __START_KERNEL_map _AC(0xffffffff80000000, UL)
```

`__PAGE_OFFSET` — начало прямой карты физической памяти.

#### Пример разбора адреса

Возьмём адрес `0xffffffff81000000`:

```text
1111111111111111 111111111 111111110 000001000 000000000 000000000000
      63:48        47:39     38:30     29:21     20:12      11:0
```

| Часть | Значение |
| --- | --- |
| 63:48 | `0xffff` — не используются, знак ядра |
| 47:39 | индекс 1 в PML4 |
| 38:30 | индекс 1 в PDPT |
| 29:21 | индекс 2 в PD |
| 20:12 | индекс 8 в PT |
| 11:0 | смещение 0 |

Откуда взялся этот адрес: kernel text mapping начинается с
`0xffffffff80000000`, а `CONFIG_PHYSICAL_START` равен `0x1000000`
(16 МБ). Сумма даёт `0xffffffff81000000`, что видно в `readelf`:

```text
$ readelf -s vmlinux | grep ffffffff81000000
     1: ffffffff81000000     0 SECTION LOCAL  DEFAULT    1
 65099: ffffffff81000000     0 NOTYPE  GLOBAL DEFAULT    1 _text
 90766: ffffffff81000000     0 NOTYPE  GLOBAL DEFAULT    1 startup_64
```

### ELF: исполняемый и линкуемый формат

#### Три части файла

ELF (Executable and Linkable Format) — стандартный формат для
исполняемых файлов, объектного кода, разделяемых библиотек и core dump.
Его используют Linux и большинство UNIX-подобных систем.

| Часть | Назначение |
| --- | --- |
| ELF header | тип, архитектура, адрес входа, смещения остальных частей |
| Program header table | список сегментов с атрибутами — нужна загрузчику |
| Section header table | описание секций |

Разница принципиальна: **программа видит сегменты, линкер — секции**.

#### ELF header

Расположен в начале файла, его задача — найти все остальные части.
Структура `Elf64_Ehdr` из `include/uapi/linux/elf.h`:

```C
typedef struct elf64_hdr {
    unsigned char    e_ident[EI_NIDENT];
    Elf64_Half e_type;      /* тип объекта */
    Elf64_Half e_machine;   /* архитектура */
    Elf64_Word e_version;
    Elf64_Addr e_entry;     /* виртуальный адрес входа */
    Elf64_Off e_phoff;      /* смещение program headers */
    Elf64_Off e_shoff;      /* смещение section headers */
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;    /* размер самого заголовка */
    Elf64_Half e_phentsize; /* размер одной записи PH */
    Elf64_Half e_phnum;     /* число program headers */
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;     /* число section headers */
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;
```

`e_ident` содержит магию `7f 45 4c 46` и общие признаки формата —
по ней файл опознаётся как ELF.

#### Секции и сегменты

Секции содержат сами данные, их заголовок — `Elf64_Shdr`:

```C
typedef struct elf64_shdr {
    Elf64_Word sh_name;        /* имя секции */
    Elf64_Word sh_type;        /* тип */
    Elf64_Xword sh_flags;      /* атрибуты */
    Elf64_Addr sh_addr;        /* виртуальный адрес */
    Elf64_Off sh_offset;       /* смещение в файле */
    Elf64_Xword sh_size;       /* размер */
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;  /* выравнивание */
    Elf64_Xword sh_entsize;    /* размер элемента, если таблица */
} Elf64_Shdr;
```

В исполняемом файле секции сгруппированы в сегменты, их заголовок —
`Elf64_Phdr`:

```C
typedef struct elf64_phdr {
    Elf64_Word p_type;         /* LOAD, NOTE и т.д. */
    Elf64_Word p_flags;        /* R, W, E */
    Elf64_Off p_offset;        /* где в файле */
    Elf64_Addr p_vaddr;        /* куда маппить в памяти */
    Elf64_Addr p_paddr;        /* физический адрес */
    Elf64_Xword p_filesz;      /* размер в файле */
    Elf64_Xword p_memsz;       /* размер в памяти */
    Elf64_Xword p_align;
} Elf64_Phdr;
```

Ключевое различие `p_filesz` и `p_memsz`: первый — сколько байт
занимает сегмент в файле, второй — сколько в памяти. Разница
покрывается нулями; в ней, например, живёт `.bss`.

#### vmlinux

`vmlinux` — перемещаемый ELF-объект. Заголовок:

```text
$ readelf -h vmlinux
ELF Header:
  Magic:   7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00
  Class:                             ELF64
  Data:                              2's complement, little endian
  Type:                              EXEC (Executable file)
  Machine:                           Advanced Micro Devices X86-64
  Entry point address:               0x1000000
  Start of program headers:          64 (bytes into file)
  Size of program headers:           56 (bytes)
  Number of program headers:         5
  Number of section headers:         73
```

Обрати внимание: `Entry point address` равен `0x1000000` — это
`CONFIG_PHYSICAL_START`, а не виртуальный адрес. Виртуальный адрес
появится после линковки.

#### Почему адрес не 0xffffffff80000000

В `arch/x86/kernel/vmlinux.lds.S`:

```text
    . = __START_KERNEL;
    .text : AT(ADDR(.text) - LOAD_OFFSET) {
    _text = .;
    }
```

где

```C
#define __START_KERNEL (__START_KERNEL_map + __PHYSICAL_START)
```

`__START_KERNEL_map` = `0xffffffff80000000` (начало kernel text mapping),
`__PHYSICAL_START` = `0x1000000`. Отсюда `0xffffffff81000000` — тот самый
адрес `startup_64`, который мы видели в paging. Конструкция
`AT(ADDR(.text) - LOAD_OFFSET)` задаёт, по какому физическому адресу
данные окажутся в файле.

#### Сегменты vmlinux

```text
$ readelf -l vmlinux
There are 5 program headers, starting at offset 64

Program Headers:
  Type           Offset             VirtAddr           PhysAddr
                 FileSiz            MemSiz              Flags  Align
  LOAD           0x0000000000200000 0xffffffff81000000 0x0000000001000000
                 0x0000000000cfd000 0x0000000000cfd000  R E    200000
  LOAD           0x0000000001000000 0xffffffff81e00000 0x0000000001e00000
                 0x0000000000100000 0x0000000000100000  RW     200000
  LOAD           0x0000000001200000 0x0000000000000000 0x0000000001f00000
                 0x0000000000014d98 0x0000000000014d98  RW     200000
  LOAD           0x0000000001315000 0xffffffff81f15000 0x0000000001f15000
                 0x000000000011d000 0x0000000000279000  RWE    200000
  NOTE           0x0000000000b17284 0xffffffff81917284 0x0000000001917284

  Section to Segment mapping:
   Segment Sections...
    00     .text .notes __ex_table .rodata __bug_table ...
    01     .data .vvar
    02     .data..percpu
    03     .init.text .init.data .exit.text .bss .brk
```

`VirtAddr` (куда маппить) и `PhysAddr` (где в файле) — разные числа,
причём разница постоянна. Флаги `R E` и `RW` — права сегмента. Обрати
внимание на третий LOAD: `VirtAddr` равен нулю, а `p_memsz` больше
`p_filesz` — это и есть `.bss`.

### Inline assembly

#### Basic и extended формы

В коде ядра постоянно встречается:

```C
__asm__("andq %%rsp,%0; ":"=r" (ti) : "0" (CURRENT_MASK));
```

Это inline assembly — ассемблерный код, встроенный в язык высокого
уровня. GCC поддерживает две формы.

**Basic** — только ключевое слово и строки инструкций:

```C
__asm__("movq %rax, %rsp");
__asm__("hlt");
```

```C
__asm__("movq $3, %rax\t\n"
        "movq %rsi, %rdi");
```

`__asm__` переносим, `asm` — расширение GNU, поэтому в ядре используют
первый вариант.

**Extended** — с операндами, и именно она даёт возможность передавать
параметры, делать переходы по меткам и сообщать компилятору о
побочных эффектах:

```text
__asm__ [volatile] [goto] (AssemblerTemplate
                           [ : OutputOperands ]
                           [ : InputOperands  ]
                           [ : Clobbers       ]
                           [ : GotoLabels     ]);
```

В квадратных скобках всё необязательное. Уберём их — получим basic
форму.

#### Квалификаторы

**`volatile`** запрещает оптимизации вокруг оператора и требует поставить
его ровно туда, где написан:

```C
static inline void native_load_gdt(const struct desc_ptr *dtr)
{
    asm volatile("lgdt %0"::"m" (*dtr));
}
```

Здесь важен порядок: если компилятор переставит инструкцию, в `GDTR`
попадёт не готовый адрес таблицы дескрипторов — ядро не загрузится.

**`goto`** сообщает, что ассемблерный код может перепрыгнуть на одну из
указанных меток:

```C
__asm__ goto("jmp %l[label]" : : : : label);
```

#### Четыре части тела

| Часть | Содержимое |
| --- | --- |
| `AssemblerTemplate` | строки инструкций, разделённые `\t\n` |
| `OutputOperands` | что ассемблер пишет |
| `InputOperands` | что ассемблер читает |
| `Clobbers` | регистры, изменённые побочно |

В extended-форме имена регистров пишутся с `%%`, непосредственные
значения — с `$`. Операнды в шаблоне обозначаются `%N`, где `N` —
номер операнда, считая с нуля.

#### Пример с разбором

```C
unsigned long a = 5, b = 10, sum = 0;

__asm__("addq %1,%2" : "=r" (sum) : "r" (a), "0" (b));
```

`addq %1,%2` складывает первый и второй операнды и кладёт результат во
второй.

**Output** `"=r" (sum)`: constraint `r` говорит «положи значение в
general purpose register», а `=` — modifier, означающий, что старое
значение выбрасывается.

**Modifier'ы:**

| Символ | Значение |
| --- | --- |
| `=` | только вывод, старое значение не читается |
| `+` | операнд и читается, и пишется |
| `&` | выходной регистр не должен совпадать со входными, только для вывода |
| `%` | операнды коммутативны, компилятор может менять местами |

**Input** `"r" (a), "0" (b)`: цифра `0` — matching constraint, она
заставляет компилятор использовать **тот же** регистр, что и операнд
`sum`. Без этого `b` и результат оказались бы в разных регистрах и
сложение не сработало бы.

Компилятор кладёт `a` в `%rdx`, `b` — в `%rax`, и складывает на
месте.

`b` и результат в одном регистре `%rax` — как и требовалось.

#### Clobbers

Список изменённых регистров. Если не указать clobber, компилятор может
использовать этот регистр у себя и получить неверный код:

```C
__asm__("movq $100, %%rdx\t\n"
        "addq %1,%2" : "=r" (sum) : "r" (a), "0" (b));
```

Здесь `%rdx` затёрт, и `a` окажется в `%rdx` — сложится 100, а не 10.
С добавлением clobber:

```C
__asm__("movq $100, %%rdx\t\n"
        "addq %1,%2" : "=r" (sum) : "r" (a), "0" (b) : "%rdx");
```

компилятор переносит `a` в `%rcx` и портит уже свободный `%rdx` —
семантика восстановлена.

Помимо регистров есть два специальных clobber'а.

**`cc`** — изменяются флаги:

```C
__asm__("incq %0" ::""(variable): "cc");
```

**`memory`** — ассемблерный код читает и пишет память, не указанную в
операndах. Это ловит ошибку оптимизатора:

```C
unsigned long a[3] = {10000000000, 0, 1};
unsigned long b = 5;

__asm__ volatile("incq %0" :: "m" (a[0]));
printf("a[0] - b = %lu\n", a[0] - b);
```

С `-O3` выводится `9999999995`, хотя правильный ответ `9999999996`:
компилятор посчитал `a[0] - 5` во время компиляции и подставил
константу, полностью проигнорировав `incq` (видно
`movabs $0x2540be3fb` вместо перечитывания памяти).

С добавлением `"memory"`:

```C
__asm__ volatile("incq %0" :: "m" (a[0]) : "memory");
```

Теперь `a[0]` перечитывается в `%rax`, и вычитание `-0x5` делается
в runtime.

Правило простое: если ассемблер трогает память, а компилятор об этом
не знает — нужен `"memory"`.

#### Constraints

| Constraint | Значение |
| --- | --- |
| `r` | general purpose register |
| `m` | операнд в памяти, ассемблеру передаётся адрес |
| `i` | непосредственная целая константа произвольного размера |
| `I` | непосредственная 32-битная константа |
| `J` | константа 0…63 |
| `K` | константа со знаком, 8 бит |
| `N` | константа без знака, 8 бит |
| `o` | адрес в памяти со смещением |
| `0`–`9` | matching constraint, тот же регистр, что и у операнда N |

Разница между `i` и `I` существенна.

С constraint `I` значение `0xffffffffffff` даёт
`error: impossible constraint in 'asm'` — оно не помещается в 32 бита.
С `i` вместо `I` тот же код компилируется.

Constraint'ы можно комбинировать, если они не конфликтуют:
`__asm__ ("movq %1,%0" : "=mr"(b) : "rm"(a));` — компилятор сам выберет
выгодный вариант.

#### Архитектурные constraints

Специфичные для x86_64:

| Constraint | Регистр |
| --- | --- |
| `a` | `%al`, `%ax`, `%eax`, `%rax` — по размеру операнда |
| `b` | `%bl`, `%bx`, `%ebx`, `%rbx` |
| `c` | `%cl`, `%cx`, `%ecx`, `%rcx` |
| `d` | `%dl`, `%dx`, `%edx`, `%rdx` |
| `S` | `%si`, `%esi`, `%rsi` |
| `D` | `%di`, `%edi`, `%rdi` |
| `f` | любой регистр стека FPU, `%st` |
| `t` | вершина стека FPU |
| `u` | второе значение с вершины стека FPU |

Ограничение `d` принудительно кладёт значение в `%rdx`:
`__asm__ ("movq %1,%0" : "=r"(b) : "d"(a));` кладёт `a` именно в `%rax`,
как и требует `d`.

---

## Памятка

| Вопрос | Ответ |
| --- | --- |
| `HZ` | частота тиков, равна `CONFIG_HZ`, на x86_64 дефолт 1000 |
| `jiffies` | число тиков с загрузки, растёт на каждом прерывании |
| `refined_jiffies` | jiffies по частоте PIT 1 193 182 Гц, rating 2 |
| Формула перевода в нс | `ns = (cycles * mult) >> shift` |
| Кто выбирает источник | `clocksource_select`, по наибольшему rating |
| Что значит rating 300–399 | быстрый и точный источник |
| Что делает tick broadcast | будит CPU с остановленным таймером |
| `CONFIG_NO_HZ_IDLE` | пропускать тики на idle-процессорах |
| `CONFIG_NO_HZ_FULL` | пропускать тики, если задача одна |
| `housekeeping_mask` | процессоры, которые не уходят в NO_HZ |
| Где живут таймеры | `tvec_bases`, per-cpu, каскад `tv1`–`tv5` |
| Когда выполняются таймеры | в softirq `TIMER_SOFTIRQ`, не в обработчике IRQ |
| `CLOCK_EVT_FEAT_ONESHOT` | устройство даёт одно событие |
| clocksource vs clockevents | «который час» против «когда позвать» |
| Rating на x86_64 | tsc 300, hpet 250, acpi_pm 200 |
| Где смотреть источник | `/sys/devices/system/clocksource/clocksource0/` |
| Где смотреть uptime | `CLOCK_BOOTTIME` или `/proc/uptime` |
| Что такое gtod | `vsyscall_gtod_data`, данные времени для vDSO |
| `TASK_INTERRUPTIBLE` | сон с возможностью прерывания сигналом |
| Размер страницы | 4096 байт, 512 записей в таблице |
| Сколько уровней paging | 4 на x86_64 |
| Значимые биты адреса | младшие 48, остальное — sign extension |
| PML4 | верхний уровень, лежит в `cr3` |
| `__PAGE_OFFSET` | `0xffff880000000000`, начало прямой карты RAM |
| Разделы ELF | header, program headers (сегменты), section headers |
| Кто читает ELF | загрузчик — сегменты, линкер — секции |
| `p_filesz` vs `p_memsz` | размер в файле против размера в памяти |
| `__START_KERNEL` | `__START_KERNEL_map + __PHYSICAL_START` |
| Inline asm части | template, outputs, inputs, clobbers |
| `=`, `+`, `&` | только вывод, чтение и запись, отдельный регистр |
| `r`, `m`, `i` | регистр, память, непосредственная константа |
| `0`–`9` | matching constraint, тот же регистр |
| `cc` clobber | ассемблер меняет флаги |
| `memory` clobber | ассемблер трогает память, иначе врежется оптимизатор |

---

> **Связанные конспекты:**
> [Прерывания Linux](linux-interrupts.md),
> [Системные вызовы](linux-syscall.md),
> [Управление памятью и сборка](linux-mm-build-link.md),
> [Инициализация ядра и структуры данных](linux-init-data-structures.md),
> [CPU и cgroups](linux-cpu-cgroups.md),
> [Примитивы синхронизации](linux-sync-primitives.md).
