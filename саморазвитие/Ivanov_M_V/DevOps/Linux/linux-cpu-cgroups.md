# Ядро Linux: процессоры, initcall, уведомления и cgroups

> Конспективное изложение глав книги `linux-insides`
> (`linux-cpu-1`–`linux-cpu-4`, `linux-cgroups-1`).
> Суть, ключевые таблицы и команды — без полного разбора исходного кода.

---

## 📌 Оглавление

- [Процессорные переменные (per-cpu)](#процессорные-переменные-per-cpu)
- [Маски процессора (cpumasks)](#маски-процессора-cpumasks)
- [Механизм initcall](#механизм-initcall)
- [Цепочки уведомлений (notifier chains)](#цепочки-уведомлений-notifier-chains)
- [Контрольные группы (cgroups)](#контрольные-группы-cgroups)
- [Шпаргалка команд](#шпаргалка-команд)

---

## Процессорные переменные (per-cpu)

**Процессорная переменная** — переменная, у которой **каждое ядро процессора
(CPU) имеет свою собственную копию**. Доступ к своей копии не требует
блокировок.

### Объявление

API ядра — макрос `DEFINE_PER_CPU` из `include/linux/percpu-defs.h`:

```c
DEFINE_PER_CPU(int, per_cpu_n)
```

После раскрытия вложенных макросов получается обычная глобальная переменная
в специальной секции ELF:

```c
__attribute__((section(".data..percpu"))) int per_cpu_n
```

Секция `.data..percpu` видна в бинарнике `vmlinux`.

### Инициализация при загрузке

Функция `setup_per_cpu_areas()` (вызывается из `init/main.c`, определена в
`arch/x86/kernel/setup_percpu.c`) **копирует секцию `.data..percpu` по одному
разу на каждый процессор**.

Вывод виден в логе ядра:

```bash
dmesg | grep percpu
# setup_percpu: NR_CPUS:8 nr_cpumask_bits:8 nr_cpu_ids:8 nr_node_ids:1
```

`NR_CPUS` — предел количества процессоров из конфига ядра `CONFIG_NR_CPUS`,
`nr_node_ids` — число узлов NUMA.

### Аллокатор первого блока

Весь per-cpu объём выделяется блоками. Тип первого блока выбирается
параметром командной строки ядра `percpu_alloc`:

| Значение | Что делает |
| ---------- | ------------ |
| `embed` | Встраивает первую per-cpu область в bootmem (через memblock) |
| `page` | Сопоставляет первую область со страницами `PAGE_SIZE` |
| `auto` | Значение по умолчанию (сейчас выбрано `embed`) |

Параметры `pcpu_embed_first_chunk`: резерв под статические переменные,
`dyn_size` (минимум под динамическое выделение), `atom_size` (кратность
выравнивания, `PMD_SIZE` на x86_64), расстояние между процессорами и
функции выделения/освобождения страниц.

### Доступ к переменной

| Макрос | Что делает |
| -------- | ------------ |
| `get_cpu_var(var)` | Отключает вытеснение, копия текущего CPU |
| `put_cpu_var(var)` | Возвращает вытеснение (`preempt_enable`) |
| `per_cpu_ptr(ptr, cpu)` | Указатель на копию переменной для CPU |

Вытеснение отключается потому, что ядро **вытесняемое**: без этого задача
могла бы «переехать» на другой процессор посреди доступа к «своей» копии.

`per_cpu_ptr` работает через массив смещений:

```c
#define per_cpu_offset(x) (__per_cpu_offset[x])
```

Массив `__per_cpu_offset[NR_CPUS]` заполняется расстояниями между копиями
`.data..percpu`. Указатель = адрес переменной + смещение её копии.

Типичное использование:

```c
get_cpu_var(var);
// работа с var
put_cpu_var(var);
```

### Итог по алгоритму

1. При инициализации ядро создаёт N секций `.data..percpu` (по одной на CPU).
2. Переменные из `DEFINE_PER_CPU` лежат в первой секции (CPU0).
3. `__per_cpu_offset` заполняется расстояниями между секциями.
4. `per_cpu_ptr(ptr, cpu)` прибавляет нужное смещение — получаем копию
   переменную для конкретного процессора.

---

## Маски процессора (cpumasks)

**Cpumask** — битовая маска, где **один бит = один номер процессора**.
Хранит состояние всех CPU системы.

Файлы: `include/linux/cpumask.h`, `lib/cpumask.c`, `kernel/cpu.c`.

### Четыре маски состояния

| Маска | Значение |
| ------- | ---------- |
| `cpu_possible` | Максимум возможных процессоров (`NR_CPUS`) |
| `cpu_present` | Процессоры, **физически подключённые** сейчас |
| `cpu_online` | Подмножество `present`, доступны планировщику |
| `cpu_active` | На эти процессоры ядро **может перемещать задачи** |

- При отключённом `CONFIG_HOTPLUG_CPU`: `possible == present`,
  `active == online`.
- Первый загрузочный процессор переводится во все состояния сразу
  (`boot_cpu_init`):

```c
set_cpu_online(cpu, true);
set_cpu_active(cpu, true);
set_cpu_present(cpu, true);
set_cpu_possible(cpu, true);
```

### Как задаётся маска

```c
typedef struct cpumask {
    DECLARE_BITMAP(bits, NR_CPUS);
} cpumask_t;
```

`DECLARE_BITMAP(name, bits)` создаёт массив `unsigned long` нужной
длины. `NR_CPUS` берётся из опции конфигурации `CONFIG_NR_CPUS`.

### API масок

| Функция/макрос | Действие |
| ---------------- | ---------- |
| `set_cpu_online(cpu, bool)` | Помечает CPU онлайн/offline |
| `cpumask_set_cpu` / `cpumask_clear_cpu` | Ставит/снимает бит процессора |
| `cpumask_test_cpu` | Проверяет, установлен ли бит |
| `num_online_cpus()` | Сколько процессоров онлайн |
| `for_each_cpu` | Перебор всех CPU в маске |
| `cpumask_setall` | Включает все биты |
| `cpumask_size` | Размер `struct cpumask` в байтах |

### Почему это атомарно

`cpumask_set_cpu` вызывает `set_bit`, который на x86 выполняет
инструкцию `bts` с префиксом **блокировки** (`LOCK_PREFIX`): бит
выставляется **атомарно**, даже если несколько CPU работают с маской
одновременно.

---

## Механизм initcall

**Initcall** — способ ядра вызывать функции инициализации **в строго
определённом порядке**, чтобы подсистемы успевали зависеть друг от друга
(например, каталог `debugfs` создаётся раньше, чем файлы внутри него).

Примеры «вешания» initcall на функцию:

```c
arch_initcall(init_pit_clocksource);
early_param("debug", debug_kernel);
```

### Уровни инициализации

| Уровень | Макрос | Порядок |
| --------- | -------- | --------- |
| Ранний | `early_initcall` | 0 |
| Ядро | `core_initcall` | 1 |
| После ядра | `postcore_initcall` | 2 |
| Архитектура | `arch_initcall` | 3 |
| Подсистемы | `subsys_initcall` | 4 |
| Файловые системы | `fs_initcall` | 5 |
| Корневая ФС | `rootfs_initcall` | между 5 и 6 |
| Устройства | `device_initcall` | 6 |
| Поздние | `late_initcall` | 7 |

У каждого уровня есть вариант `_sync` — он ждёт завершения всех
процессов инициализации предыдущего уровня.

Макросы определены в `include/linux/init.h` и сводятся к
`__define_initcall(fn, id)`, который кладёт указатель на функцию в
секцию `.initcall<id>.init`. Секции собирает линковочный скрипт в
`.init.data` между метками `__initcall_start` и `__initcall_end`.

### Как вызываются

Цепочка запуска:

```text
do_basic_setup()  →  do_initcalls()  →  do_initcall_level(level)
                                        →  do_one_initcall(fn)
```

- `do_initcalls` перебирает уровни 0..7 по массиву `initcall_levels`.
- `do_one_initcall`:
  - пропускает initcall из **чёрного списка** (`initcall_blacklisted`,
    список формируется из командной строки ядра);
  - выполняет回调 (в debug-режиме — через `do_one_initcall_debug`);
  - проверяет баланс счётчика вытеснения `preempt_count` и состояние
    прерываний `irqs_disabled` — и «лечит» дисбаланс с предупреждением.

### Отладка

Параметр командной строки ядра `initcall_debug` — трейс каждого вызова
в журнал ядра (имя функции, PID, длительность в микросекундах):

```text
initcall_debug  [KNL] Trace initcalls as they are executed. Useful
for working out where the kernel is dying during startup.
```

Пример работы уровня `rootfs` — распаковка initramfs
(`rootfs_initcall(populate_rootfs)`), видна в логе как:

```text
[    0.199960] Unpacking initramfs...
```

---

## Цепочки уведомлений (notifier chains)

**Notifier chain** — механизм «издатель/подписчик» **внутри ядра**:
подсистема подписывается на асинхронные события другой подсистемы.
Для общения с пользовательским пространством это не предназначено.

Файлы: `include/linux/notifier.h`, `kernel/notifier.c`.

### Тип回调а и коды ответа

```c
typedef int (*notifier_fn_t)(struct notifier_block *nb,
                             unsigned long action, void *data);
```

| Код | Значение |
| ----- | ---------- |
| `NOTIFY_DONE` (0x0000) | Подписчик не заинтересован |
| `NOTIFY_OK` (0x0001) | Уведомление обработано |
| `NOTIFY_BAD` | Что-то пошло не так |
| `NOTIFY_STOP` | Обработано, но **останавливать** дальнейшие вызовы |

### Основная структура

```c
struct notifier_block {
    notifier_fn_t notifier_call;   // сам callback
    struct notifier_block __rcu *next; // следующий в цепочке
    int priority;                  // выше приоритет — раньше вызов
};
```

### Четыре типа цепочек

| Тип | Контекст | Защита |
| ----- | ---------- | -------- |
| Blocking | Контекст процесса (можно блокироваться) | `rw_semaphore` |
| SRCU | Контекст процесса | особый RCU, можно блокировать на чтении |
| Atomic | Прерывания / атомарный контекст | `spinlock` |
| Raw | Без ограничений | на стороне вызывающего |

### API

| Действие | Функции |
| ---------- | --------- |
| Инициализация заголовка | `BLOCKING_INIT_NOTIFIER_HEAD` и аналоги |
| Подписка | `blocking_notifier_chain_register` и аналоги для других типов |
| Отписка | `blocking_notifier_chain_unregister` и аналоги |
| Уведомление | `blocking_notifier_call_chain` и аналоги |

Аналогичные макросы/функции для остальных типов:
`ATOMIC_INIT_NOTIFIER_HEAD`, `RAW_INIT_NOTIFIER_HEAD`,
`srcu_init_notifier`.

При регистрации `notifier_block` вставляется в список **по убыванию
`priority`**. Вызов уведомления: `*_call_chain` → `down_read` → обход
списка → `nb->notifier_call(nb, val, v)`.

### Пример: уведомления о модулях ядра

```c
static BLOCKING_NOTIFIER_HEAD(module_notify_list);

int register_module_notifier(struct notifier_block *nb)
{
    return blocking_notifier_chain_register(&module_notify_list, nb);
}
```

События модуля: `MODULE_STATE_LIVE`, `MODULE_STATE_COMING`,
`MODULE_STATE_GOING`. Подписывается, например, подсистема tracepoints
(`init_tracepoints` → `register_module_notifier`).

Уведомление уходит из syscall'ов: `init_module` шлёт `LIVE`/`COMING`,
а `delete_module` — `GOING`:

```c
blocking_notifier_call_chain(&module_notify_list,
                             MODULE_STATE_GOING, mod);
```

---

## Контрольные группы (cgroups)

**Cgroups** — механизм распределения ресурсов (время CPU, память, число
процессов и т.д.) между группами задач. Организованы **иерархически**:
дочерняя группа наследует параметры родителя.

Отличие от дерева процессов: **иерархий cgroup может быть много
одновременно**, и каждая привязана к набору подсистем.

### Подсистемы (12 штук)

| Подсистема | Что регулирует |
| ------------ | ---------------- |
| `cpuset` | Назначает CPU и узлы памяти задачам группы |
| `cpu` | Доступ задач к ресурсам процессора (через планировщик) |
| `cpuacct` | Отчёт об использовании процессора |
| `io` (blkio) | Ограничение чтения/записи блочных устройств |
| `memory` | Ограничение использования памяти |
| `devices` | Доступ задач к устройствам |
| `freezer` | Приостановка/возобновление задач |
| `net_cls` | Пометка сетевых пакетов группы |
| `net_prio` | Приоритет сетевого трафика по интерфейсам |
| `perf_event` | Доступ к событиям производительности |
| `hugetlb` | Поддержка больших (huge) страниц |
| `pids` | Ограничение числа процессов в группе |

Каждая подсистема включается своей опцией конфига
(`CONFIG_CPUSETS`, `CONFIG_BLK_CGROUP`, ...) в меню
`General setup -> Control Group support`.

### Где это видно

```bash
cat /proc/cgroups
#subsys_name  hierarchy  num_cgroups  enabled
cpuset        8          1            1
cpu           7          66           1
...

ls -l /sys/fs/cgroup/   # cgroupfs смонтирован здесь
```

### Как создать cgroup

1. Способ «руками»: `mkdir` подкаталога в нужной подсистеме
   (`/sys/fs/cgroup/<subsys>/mygroup`) и записать PID в файл `tasks`
   (создаётся автоматически).
2. Способ утилитами: библиотека `libcgroup`
   (`libcgroup-tools` в Fedora) — создание/удаление/управление.

### Пример: запрет записи в устройство

Скрипт каждые 5 секунд пишет строку в `/dev/tty`:

```bash
while :; do echo "print line" > /dev/tty; sleep 5; done
```

Создаём группу и запрещаем запись в `/dev/tty`:

```bash
cd /sys/fs/cgroup/devices
mkdir cgroup_test_group
echo "c 5:0 w" > cgroup_test_group/devices.deny
echo $(pidof -x cgroup_test_script.sh) \
    > cgroup_test_group/tasks
```

Расшифровка правила `c 5:0 w`:

| Часть | Значение |
| ------- | ---------- |
| `c` | Тип устройства — символьное (`ls -l` покажет `c` в правах) |
| `5:0` | Старший и младший номера устройства (`major:minor`) |
| `w` | Запретить **запись** (write) |

После добавления PID скрипт получает:

```text
./cgroup_test_script.sh: line 5: /dev/tty: Operation not permitted
```

### Docker и cgroups

Каждый контейнер получает свою cgroup — PID контейнера видны в
`/sys/fs/cgroup/devices/docker/<id>/tasks`. Дерево можно посмотреть:

```bash
systemd-cgls
# Control group /:
# -.slice
# ├─docker
# │ └─<id>
# │   ├─5501 mysqld
# │   └─6404 /bin/bash
```

### Ранняя инициализация в ядре

Делится на **раннюю** (`cgroup_init_early`, вызывается из `init/main.c`,
определена в `kernel/cgroup.c`) и **позднюю**.

Что делает `cgroup_init_early`:

1. Инициализирует стандартную объединённую иерархию
   (`init_cgroup_root(&cgrp_dfl_root, &opts)`), отключает подсчёт
   ссылок флагом `CSS_NO_REF`.
1. Привязывает `css_set` к первому процессу системы:

   ```c
   RCU_INIT_POINTER(init_task.cgroups, &init_css_set);
   ```

1. Перебирает подсистемы (`for_each_subsys`), назначает `id` и `name`,
   а для подсистем с `early_init = true` вызывает
   `cgroup_init_subsys` — корень иерархии, выделение через
   `css_alloc`, связь с родителем.

**Ранние подсистемы:** `cpuset`, `cpu`, `cpuacct`.

Опции монтирования cgroupfs описывает структура `cgroup_sb_opts`,
например создание именованной иерархии без подсистем:

```bash
mount -t cgroup -oname=my_cgrp,none /mnt/cgroups
```

### Связь структур данных

`task_struct` не хранит прямой ссылки на свою cgroup — цепочка такая:

```text
task_struct ──cgroups──▶ css_set ──subsys[]──▶ cgroup_subsys_state
                                                    │
                                                    ▼
                                                  cgroup
                                                    │
                                                    ▼
                                             cgroup_subsys
                                    (id, name, css_online, css_offline)
```

---

## Шпаргалка команд

| Команда | Что показывает |
| --------- | ---------------- |
| `dmesg \| grep percpu` | Инициализацию per-cpu областей (NR_CPUS и т.д.) |
| `cat /proc/cgroups` | Подсистемы cgroup, их hierarchy и счётчики |
| `ls -l /sys/fs/cgroup/` | Смонтированные иерархии cgroup |
| `echo PID > .../tasks` | Прикрепление процесса к cgroup |
| `echo "c 5:0 w" > devices.deny` | Запрет записи в устройство для группы |
| `systemd-cgls` | Дерево процессов по control group |
| `initcall_debug` в cmdline | Трейс порядка initcall в dmesg |
