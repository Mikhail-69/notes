# Примитивы синхронизации в ядре Linux

> Конспект главы `SyncPrim` книги `linux-insides`
> (`linux-sync-1`–`linux-sync-6`, исходники на английском — переведены).
> Спинлоки, queued spinlocks, семафоры, мьютексы, RW-семафоры и
> seqlock: теория, структуры данных, API и разбор путей захвата.
> Общие материалы по ядру — в конспекте
> [здесь](Конспект_ядро_Linux.md).

---

## 📌 Оглавление

- [Что такое синхронизация](#что-такое-синхронизация)
- [Спинлоки](#спинлоки)
  - [Структура spinlock_t](#структура-spinlock_t)
  - [Основные операции](#основные-операции)
  - [Инициализация spin_lock_init](#инициализация-spin_lock_init)
  - [Захват: spin_lock](#захват-spin_lock)
  - [Путь захвата до arch_spin_lock](#путь-захвата-до-arch_spin_lock)
- [Queued spinlocks](#queued-spinlocks)
  - [Проблемы обычного спинлока](#проблемы-обычного-спинлока)
  - [Алгоритм MCS](#алгоритм-mcs)
  - [Устройство qspinlock](#устройство-qspinlock)
  - [API и быстрый путь](#api-и-быстрый-путь)
  - [Медленный путь slowpath](#медленный-путь-slowpath)
- [Семафоры](#семафоры)
  - [Spinlock против семафора](#spinlock-против-семафора)
  - [Структура семафора](#структура-семафора)
  - [Инициализация семафора](#инициализация-семафора)
  - [API семафоров](#api-семафоров)
  - [Функция down](#функция-down)
  - [Функция up](#функция-up)
- [Мьютексы (mutex)](#мьютексы-mutex)
  - [Отличия от семафора](#отличия-от-семафора)
  - [Структура mutex](#структура-mutex)
  - [Три пути захвата](#три-пути-захвата)
  - [Инициализация мьютекса](#инициализация-мьютекса)
  - [mutex_lock и fastpath](#mutex_lock-и-fastpath)
  - [midpath: оптимистичное вращение](#midpath-оптимистичное-вращение)
  - [slowpath мьютекса](#slowpath-мьютекса)
  - [mutex_unlock](#mutex_unlock)
  - [Дополнительное API мьютексов](#дополнительное-api-мьютексов)
- [RW-семафоры](#rw-семафоры)
  - [Идея readers/writer](#идея-readerswriter)
  - [Структура rw_semaphore](#структура-rw_semaphore)
  - [Значения поля count](#значения-поля-count)
  - [Инициализация rwsem](#инициализация-rwsem)
  - [API RW-семафоров](#api-rw-семафоров)
  - [Захват на запись: down_write](#захват-на-запись-down_write)
  - [Захват на чтение: down_read](#захват-на-чтение-down_read)
  - [Освобождение: up_write и up_read](#освобождение-up_write-и-up_read)
- [Seqlock (sequential lock)](#seqlock-sequential-lock)
  - [Идея seqlock](#идея-seqlock)
  - [Структура seqlock_t](#структура-seqlock_t)
  - [Инициализация seqlock](#инициализация-seqlock)
  - [API seqlock](#api-seqlock)
  - [Чтение: read_seqbegin и read_seqretry](#чтение-read_seqbegin-и-read_seqretry)
  - [Пример: get_jiffies_64](#пример-get_jiffies_64)
  - [Запись: write_seqlock](#запись-write_seqlock)
- [Шпаргалка](#шпаргалка)

---

## Что такое синхронизация

**Синхронизация** — механизм, который не даёт двум и более параллельным
процессам или потокам выполняться одновременно на одном участке кода.
Пример из `kernel/time/clocksource.c` (функция
`__clocksource_register_scale` — регистрация нового clocksource):

```c
mutex_lock(&clocksource_mutex);
...
clocksource_enqueue(cs);
clocksource_enqueue_watchdog(cs);
clocksource_select();
...
mutex_unlock(&clocksource_mutex);
```

Код заключён в `mutex_lock`/`mutex_unlock`, чтобы два потока не
зарегистрировали часовой источник одновременно. `clocksource_enqueue`
вставляет элемент в общий список `clocksource_list` после источника с
наивысшим rating:

```c
static void clocksource_enqueue(struct clocksource *cs)
{
        struct list_head *entry = &clocksource_list;
        struct clocksource *tmp;

        list_for_each_entry(tmp, &clocksource_list, list) {
                if (tmp->rating < cs->rating)
                        break;
                entry = &tmp->list;
        }
        list_add(&cs->list, entry);
}
```

Если два процесса выполнят это одновременно, оба могут найти один и тот
же `entry` — возникнет **race condition**: второй процесс перезапишет
clocksource первого.

Примитивы синхронизации в ядре Linux:

- `mutex`;
- семафоры (`semaphores`);
- seqlock;
- атомарные операции;
- спинлоки (`spinlocks`) — с них мы начнём.

---

## Спинлоки

**Спинлок** — низкоуровневый механизм: переменная в двух состояниях
(`acquired` / `released`). Процесс, захватывающий блокировку, записывает
состояние «захвачено», пока владелец не снимет её. Все операции должны
быть **атомарными**. В ядре спинлок представлен типом `spinlock_t`.

### Структура spinlock_t

Определён в `include/linux/spinlock_types.h`:

```c
typedef struct spinlock {
        union {
              struct raw_spinlock rlock;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define LOCK_PADSIZE (offsetof(struct raw_spinlock, dep_map))
                struct {
                        u8 __padding[LOCK_PADSIZE];
                        struct lockdep_map dep_map;
                };
#endif
        };
} spinlock_t;
```

Без `CONFIG_DEBUG_LOCK_ALLOC` остаётся union с одним полем `rlock`.
Структура `raw_spinlock` («обычный» спинлок):

```c
typedef struct raw_spinlock {
        arch_spinlock_t raw_lock;
#ifdef CONFIG_DEBUG_SPINLOCK
        unsigned int magic, owner_cpu;
        void *owner;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
        struct lockdep_map dep_map;
#endif
} raw_spinlock_t;
```

`arch_spinlock_t` — архитектурная реализация. Для x86_64 это
`qspinlock` из `include/asm-generic/qspinlock_types.h` (разбор — в
разделе про queued spinlocks):

```c
typedef struct qspinlock {
        union {
                atomic_t val;
                struct {
                        u8      locked;
                        u8      pending;
                };
                struct {
                        u16     locked_pending;
                        u16     tail;
                };
        };
} arch_spinlock_t;
```

### Основные операции

| Операция | Что делает |
| --- | --- |
| `spin_lock_init` | инициализация спинлока |
| `spin_lock` | захват |
| `spin_lock_bh` | отключает программные прерывания и захватывает |
| `spin_lock_irqsave` / `spin_lock_irq` | отключает прерывания и захватывает |
| `spin_unlock` | освобождение |
| `spin_unlock_bh` | освобождение и включение программных прерываний |
| `spin_is_locked` | текущее состояние |

`spin_lock_irqsave` дополнительно сохраняет прежнее состояние
аппаратных прерываний в `flags`, `spin_lock_irq` — нет.

### Инициализация spin_lock_init

Макрос из `include/linux/spinlock.h`:

```c
#define spin_lock_init(_lock)                   \
do {                                            \
        spinlock_check(_lock);                  \
        raw_spin_lock_init(&(_lock)->rlock);    \
} while (0)
```

`spinlock_check` просто возвращает `rlock` (проверка, что передан
«обычный» raw-спинлок):

```c
static __always_inline raw_spinlock_t *spinlock_check(spinlock_t *lock)
{
        return &lock->rlock;
}
```

```c
# define raw_spin_lock_init(lock)               \
do {                                            \
    *(lock) = __RAW_SPIN_LOCK_UNLOCKED(lock);   \
} while (0)
```

Цепочка макросов сводится к записи нуля (debug-часть опущена):

```c
*(&(_lock)->rlock) = __ARCH_SPIN_LOCK_UNLOCKED;

#define __ARCH_SPIN_LOCK_UNLOCKED       { { .val = ATOMIC_INIT(0) } }
```

После `spin_lock_init` спинлок находится в состоянии `unlocked`.

### Захват: spin_lock

```c
static __always_inline void spin_lock(spinlock_t *lock)
{
        raw_spin_lock(&lock->rlock);
}
```

`raw_spin_lock` → `_raw_spin_lock`. Место определения зависит от
конфигурации:

- SMP выключен (`include/linux/spinlock_api_up.h`):
  `#define _raw_spin_lock(lock) __LOCK(lock)`;
- SMP включён + `CONFIG_INLINE_SPIN_LOCK`
  (`include/linux/spinlock_api_smp.h`):
  `#define _raw_spin_lock(lock) __raw_spin_lock(lock)`;
- SMP включён, инлайна нет (`kernel/locking/spinlock.c`):
  функция-обёртка над `__raw_spin_lock`.

### Путь захвата до arch_spin_lock

```c
static inline void __raw_spin_lock(raw_spinlock_t *lock)
{
        preempt_disable();
        spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
        LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}
```

1. `preempt_disable()` — отключение **прептента** (при освобождении
   прептент включается снова в `__raw_spin_unlock`), чтобы процесс не
   был переключён, пока он крутится на месте.
2. `spin_acquire` через цепочку макросов приводит к `lock_acquire` —
   работе **lockdep** (валидатор блокировок): там отключаются
   аппаратные прерывания (`raw_local_irq_save`), идёт трассировка,
   основная работа в `__lock_acquire` (`kernel/locking/lockdep.c`).
3. `LOCK_CONTENDED` просто вызывает переданную функцию:

```c
#define LOCK_CONTENDED(_lock, try, lock) \
         lock(_lock)
```

В итоге вызывается `do_raw_spin_lock`:

```c
static inline void do_raw_spin_lock(raw_spinlock_t *lock)
{
        arch_spin_lock(&lock->raw_lock);
}

#define arch_spin_lock(l)               queued_spin_lock(l)
```

То есть на x86_64 захват обычного спинлока уходит в **queued
spinlock** — тема следующего раздела.

---

## Queued spinlocks

Все `arch_*`-макросы спинлока разворачиваются в функции
`include/asm-generic/qspinlock.h`:

```c
#define arch_spin_is_locked(l)          queued_spin_is_locked(l)
#define arch_spin_is_contended(l)       queued_spin_is_contended(l)
#define arch_spin_value_unlocked(l)     queued_spin_value_unlocked(l)
#define arch_spin_lock(l)               queued_spin_lock(l)
#define arch_spin_trylock(l)            queued_spin_trylock(l)
#define arch_spin_unlock(l)             queued_spin_unlock(l)
```

`CONFIG_QUEUED_SPINLOCKS` включён по умолчанию, если задан
`ARCH_USE_QUEUED_SPINLOCKS` (`kernel/Kconfig.locks`), а тот включается
для x86_64 (`arch/x86/Kconfig`: `select ARCH_USE_QUEUED_SPINLOCKS`)
и зависит от `SMP`.

### Проблемы обычного спинлока

Обычный спинлок строится на инструкции **test and set**: записывает
значение в память и возвращает старое; вместе они атомарны.

```c
int lock(lock)
{
    while (test_and_set(lock) == 1)
        ;
    return 0;
}

int unlock(lock)
{
    lock = 0;
    return lock;
}
```

Две проблемы:

1. **Несправедливость** — поток, пришедший позже, может получить
   блокировку раньше.
2. **Инвалидация кеша** — все потоки крутятся одним `test_and_set` на
   общем адресе: кеш процессора хранит `lock=1`, а память уже `0`.

**Queued spinlock** решает обе проблемы: каждый процесс крутится на
**своей** локальной копии переменной — то есть очередь строится поверх
концепции **per-cpu** переменных.

### Алгоритм MCS

Классическая очередь — **MCS-лок** (Scott, 1991). Идея: поток
регистрируется в очереди; если блокировка свободна — забирает её,
иначе вешает свой узел на `next` предыдущего владельца и ждёт, пока
тот не снимет блокировку и не разбудит `next`.

Псевдокод:

```c
void lock(...)
{
    lock.next = NULL;
    ancestor = put_lock_to_queue_and_return_ancestor(queue, lock);

    // есть предок — блокировка занята, ждём освобождения
    if (ancestor) {
        lock.is_locked = 1;
        ancestor.next = lock;
        while (lock.is_locked == true)
                ;
    }
    // очереди не было — мы владелецы
}

void unlock(...)
{
    // уведомить следующего в очереди, если он есть
    if (lock.next != NULL)
        lock.next.is_locked = false;
    // иначе просто записать 0 и выйти
}
```

### Устройство qspinlock

Структура `qspinlock` (см. выше), поле `val` — 4 байта:

| Биты | Значение |
| --- | --- |
| 0–7 | байт `locked` |
| 8 | бит `pending` |
| 9–15 | не используются |
| 16–17 | индекс в per-cpu массиве MCS-узлов |
| 18–31 | номер процессора — хвост очереди |

`val` имеет тип `atomic_t`, поэтому все операции с ним атомарны:

```c
static __always_inline int queued_spin_is_locked(struct qspinlock *lock)
{
        return atomic_read(&lock->val);
}
```

### API и быстрый путь

```c
static __always_inline void queued_spin_lock(struct qspinlock *lock)
{
        u32 val;

        val = atomic_cmpxchg_acquire(&lock->val, 0, _Q_LOCKED_VAL);
        if (likely(val == 0))
                return;
        queued_spin_lock_slowpath(lock, val);
}
```

Атомарный `CMPXCHG`: если `val == 0` (свободно) — ставим
`_Q_LOCKED_VAL` и выходим (**fast path**). Иначе — **slow path**.
Биты `locked` и `pending`:

- `pending` означает: блокировка занята, очередь пуста, но кто-то уже
  пытается её взять. Бит ставится, чтобы не трогать per-cpu массив
  `mcs_spinlock` без нужды (лишняя инвалидация кеша даёт задержки).

### Медленный путь slowpath

`queued_spin_lock_slowpath` (`kernel/locking/qspinlock.c`):

```c
void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
        ...
        if (val == _Q_PENDING_VAL) {
                int cnt = _Q_PENDING_LOOPS;
                val = atomic_cond_read_relaxed(&lock->val,
                          (VAL != _Q_PENDING_VAL) || !cnt--);
        }
        ...
        if (val & ~_Q_LOCKED_MASK)
                goto queue;

        val = queued_fetch_set_pending_acquire(lock);

        if (unlikely(val & ~_Q_LOCKED_MASK)) {
                if (!(val & _Q_PENDING_MASK))
                        clear_pending(lock);
                goto queue;
        }

        if (val & _Q_LOCKED_MASK)
                atomic_cond_read_acquire(&lock->val,
                                         !(VAL & _Q_LOCKED_MASK));

        clear_pending_set_locked(lock);
        return;
queue:
        ...
}
```

Шаги:

1. Ожидание завершающегося захвата с ограниченным числом вращений
   (гарантия прогресса).
2. Конкуренция есть — сразу строим очередь (`goto queue`).
3. Иначе блокировка занята: ставим `pending`
   (`queued_fetch_set_pending_acquire`) и ждём владельца.
4. Если конкурентов стало больше — снимаем `pending` и идём в очередь.
5. Владелец отпустил — снимаем `pending`, ставим `locked`, выходим.

Узел очереди — `struct mcs_spinlock` (`kernel/locking/mcs_spinlock.h`):

```c
struct mcs_spinlock {
       struct mcs_spinlock *next;
       int locked;
       int count;
};
```

- `next` — следующий поток в очереди;
- `locked` — состояние текущего (`1` — занято);
- `count` — вложенные (вложенные) захваты: поток мог быть прерван
  аппаратным прерыванием, и обработчик тоже захочет блокировку.
  Поэтому на процессоре лежит **массив** узлов:

```c
static DEFINE_PER_CPU_ALIGNED(struct qnode, qnodes[MAX_NODES]);
```

Он даёт четыре попытки захвата для четырёх контекстов: обычной
задачи, аппаратного прерывания, программного прерывания и NMI.

Сборка очереди (метка `queue`):

```c
queue:
        node = this_cpu_ptr(&qnodes[0].mcs);
        idx = node->count++;
        tail = encode_tail(smp_processor_id(), idx);

        node = grab_mcs_node(node, idx);
        node->locked = 0;
        node->next = NULL;
```

Трогая свою per-cpu копию, мы даём владельцу шанс отпустить блокировку
— пробуем снова:

```c
        if (queued_spin_trylock(lock))
                goto release;
release:
        __this_cpu_dec(qnodes[0].mcs.count);
```

Не вышло — обновляем хвост очереди и забираем прежний:

```c
        old = xchg_tail(lock, tail);
        next = NULL;
```

Если очередь не пуста — связываем прежний хвост со своим узлом,
вращаемся на `locked` и оптимистично подгружаем `next`
(prefetch кеш-линии ускорит будущий MCS-unlock):

```c
        if (old & _Q_TAIL_MASK) {
                prev = decode_tail(old);
                WRITE_ONCE(prev->next, node);

                arch_mcs_spin_lock_contended(&node->locked);

                next = READ_ONCE(node->next);
                if (next)
                        prefetchw(next);
        }
```

Теперь мы — голова очереди. Осталось дождаться двух событий: пока
владелец отпустит блокировку и пока позиция `pending` не освободится:

```c
        val = atomic_cond_read_acquire(&lock->val,
                          !(VAL & _Q_LOCKED_PENDING_MASK));
```

После этого голова очереди захватывает блокировку; в конце обновляется
хвост, и текущий узел удаляется из очереди.

---

## Семафоры

### Spinlock против семафора

Спинлок при захвате останавливает все операции: **прептент
отключён**, контекстное переключение запрещено, остальные процессы
крутятся на месте (busy waiting). Поэтому спинлок годится только для
**очень коротких** критических секций. Для долгих захватов нужен
**семафор**.

Семафор — переменная, которую можно увеличивать и уменьшать; её
значение отражает доступность ресурса. Значение не ограничено 0 и 1:

- **binary semaphore** — только `0` или `1`;
- **counting semaphore** — любое неотрицательное число: больше единицы
  — блокировку могут взять несколько процессов (учёт числа
  ресурсов).

Главное отличие от спинлока: **ожидающий процесс спит**, и
планировщик может переключиться на другой процесс.

### Структура семафора

`include/linux/semaphore.h`:

```c
struct semaphore {
        raw_spinlock_t          lock;
        unsigned int            count;
        struct list_head        wait_list;
};
```

- `lock` — спинлок, защищающие данные семафора;
- `count` — число доступных ресурсов;
- `wait_list` — список ожидающих процессов.

### Инициализация семафора

Статически — только бинарный семафор, макросом `DEFINE_SEMAPHORE`:

```c
#define DEFINE_SEMAPHORE(name)  \
         struct semaphore name = __SEMAPHORE_INITIALIZER(name, 1)

#define __SEMAPHORE_INITIALIZER(name, n)              \
{                                                     \
        .lock           = __RAW_SPIN_LOCK_UNLOCKED((name).lock), \
        .count          = n,                          \
        .wait_list      = LIST_HEAD_INIT((name).wait_list),      \
}
```

`lock` переводится в `unlocked` (нулевое значение), `count` получает
заданное число ресурсов, список — пустой.

Динамически — функцией `sema_init` (в реальном коде в ней есть ещё
инициализация lockdep, опущена):

```c
static inline void sema_init(struct semaphore *sem, int val)
{
       *sem = (struct semaphore) __SEMAPHORE_INITIALIZER(*sem, val);
       /* далее — инициализация карты lockdep */
}
```

### API семафоров

```c
void down(struct semaphore *sem);
void up(struct semaphore *sem);
int  down_interruptible(struct semaphore *sem);
int  down_killable(struct semaphore *sem);
int  down_trylock(struct semaphore *sem);
int  down_timeout(struct semaphore *sem, long jiffies);
```

- `down`/`up` — захват и освобождение;
- `down_interruptible` — при неудаче переводит задачу в состояние
  `TASK_INTERRUPTIBLE`: ожидание можно прервать **сигналом**;
- `down_killable` — то же, но `TASK_KILLABLE`: прерывается только
  kill-сигналом;
- `down_trylock` — как `spin_trylock`: попытка без ожидания;
- `down_timeout` — ожидание ограничено таймаутом в **jiffies**.

### Функция down

`kernel/locking/semaphore.c`:

```c
void down(struct semaphore *sem)
{
        unsigned long flags;

        raw_spin_lock_irqsave(&sem->lock, flags);
        if (likely(sem->count > 0))
                sem->count--;
        else
                __down(sem);
        raw_spin_unlock_irqrestore(&sem->lock, flags);
}
EXPORT_SYMBOL(down);
```

Счётчик защищён спинлоком с сохранением состояния прерываний. Если
`count > 0` — уменьшаем и захватываем; иначе — спим в `__down`:

```c
static noinline void __sched __down(struct semaphore *sem)
{
        __down_common(sem, TASK_UNINTERRUPTIBLE,
                      MAX_SCHEDULE_TIMEOUT);
}
```

На том же `__down_common` построены все варианты:

```c
/* down_interruptible */
return __down_common(sem, TASK_INTERRUPTIBLE,
                     MAX_SCHEDULE_TIMEOUT);
/* down_killable */
return __down_common(sem, TASK_KILLABLE, MAX_SCHEDULE_TIMEOUT);
/* down_timeout */
return __down_common(sem, TASK_UNINTERRUPTIBLE, timeout);
```

`__down_common` создаёт текущую задачу и запись ожидания:

```c
struct task_struct *task = current;
struct semaphore_waiter waiter;

list_add_tail(&waiter.list, &sem->wait_list);
waiter.task = task;
waiter.up = false;
```

`waiter` — элемент списка `wait_list`:

```c
struct semaphore_waiter {
        struct list_head list;
        struct task_struct *task;
        bool up;
};
```

Дальше — бесконечный цикл ожидания:

```c
for (;;) {
        if (signal_pending_state(state, task))
            goto interrupted;

        if (unlikely(timeout <= 0))
            goto timed_out;

        __set_task_state(task, state);

        raw_spin_unlock_irq(&sem->lock);
        timeout = schedule_timeout(timeout);
        raw_spin_lock_irq(&sem->lock);

        if (waiter.up)
            return 0;
}
```

- Есть pending-сигнал (`signal_pending_state` проверяет биты
  `TASK_INTERRUPTIBLE | TASK_WAKEKILL` и наличие сигнала) — выходим
  с `-EINTR`:

  ```c
  interrupted:
      list_del(&waiter.list);
      return -EINTR;
  ```

- Истёк таймаут — выходим с `-ETIME`:

  ```c
  timed_out:
      list_del(&waiter.list);
      return -ETIME;
  ```

- Иначе: состояние ставится `state`, спинлок отпускается, задача
  уходит в **сон** через `schedule_timeout` (`kernel/time/timer.c`),
  спинлок берётся снова. Цикл повторяется, пока `waiter.up` не станет
  `true`.

### Функция up

Функция `up` освобождает блокировку:

```c
void up(struct semaphore *sem)
{
        unsigned long flags;

        raw_spin_lock_irqsave(&sem->lock, flags);
        if (likely(list_empty(&sem->wait_list)))
                sem->count++;
        else
                __up(sem);
        raw_spin_unlock_irqrestore(&sem->lock, flags);
}
EXPORT_SYMBOL(up);
```

Список пуст — просто увеличиваем счётчик. Иначе будим первого
ожидающего:

```c
static noinline void __sched __up(struct semaphore *sem)
{
        struct semaphore_waiter *waiter =
                list_first_entry(&sem->wait_list,
                                 struct semaphore_waiter, list);
        list_del(&waiter->list);
        waiter->up = true;
        wake_up_process(waiter->task);
}
```

Флаг `up` обрывает цикл в `__down_common`, а `wake_up_process`
(`kernel/sched/core.c`) разбудит спящую задачу.

---

## Мьютексы (mutex)

### Отличия от семафора

**Mutex** — `MUTual EXclusion`. Семантика строже, чем у семафора:

- мьютексом владеет **только один** процесс;
- разблокировать может **только владелец**;
- реализация API избегает лишнего rescheduling и дорогих
  **контекстных переключений**.

С теоретической точки зрения mutex — бинарный семафор, но реализация
в ядре другая.

### Структура mutex

`include/linux/mutex.h`:

```c
struct mutex {
        atomic_t                count;
        spinlock_t              wait_lock;
        struct list_head        wait_list;
#if defined(CONFIG_DEBUG_MUTEXES) || \
    defined(CONFIG_MUTEX_SPIN_ON_OWNER)
        struct task_struct      *owner;
#endif
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
        struct optimistic_spin_queue osq;
#endif
#ifdef CONFIG_DEBUG_MUTEXES
        void                    *magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
        struct lockdep_map      dep_map;
#endif
};
```

- `count` — состояние: `1` — `unlocked`, `0` — `locked`,
  отрицательное — `locked` с ожидающими;
- `wait_lock` — спинлок, защищающий очередь ожидания;
- `wait_list` — список ожидающих;
- `owner` — процесс-владелец (нужен для optimistic spinning);
- `osq` — очередь MCS для оптимистичного вращения;
- `magic`, `dep_map` — только отладка/lockdep.

Запись ожидания — `mutex_waiter` (похожа на `semaphore_waiter`, но
вместо `up` — поле `magic` для отладки):

```c
struct mutex_waiter {
        struct list_head        list;
        struct task_struct      *task;
#ifdef CONFIG_DEBUG_MUTEXES
        void                    *magic;
#endif
};
```

### Три пути захвата

1. **fastpath** — мьютекс свободен: атомарно уменьшаем `count`
   (освобождение — увеличиваем).
2. **midpath** (**optimistic spinning**) — мьютекс занят: вращаемся на
   MCS-очереди, пока владелец **выполняется** на CPU (и нет готовых
   задач с более высоким приоритетом). Процесс не спит и не
   переключается — экономия на контекстном переключении.
3. **slowpath** — как у семафора: добавляемся в `wait_list` и спим.

### Инициализация мьютекса

Статически:

```c
#define DEFINE_MUTEX(mutexname) \
        struct mutex mutexname = __MUTEX_INITIALIZER(mutexname)

#define __MUTEX_INITIALIZER(lockname)         \
{                                             \
       .count = ATOMIC_INIT(1),               \
       .wait_lock = __SPIN_LOCK_UNLOCKED(lockname.wait_lock), \
       .wait_list = LIST_HEAD_INIT(lockname.wait_list)        \
}
```

`count = 1` — `unlocked`.

Динамически — макросом `mutex_init`, вызывающим `__mutex_init`
(`kernel/locking/mutex.c`):

```c
void __mutex_init(struct mutex *lock, const char *name,
                  /* lockdep-параметры опущены */)
{
        atomic_set(&lock->count, 1);
        spin_lock_init(&lock->wait_lock);
        INIT_LIST_HEAD(&lock->wait_list);
        mutex_clear_owner(lock);
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
        osq_lock_init(&lock->osq);
#endif
        debug_mutex_init(lock, name, /* ... */);
}
```

`osq_lock_init` обнуляет хвост optimistic-очереди
(`include/linux/osq_lock.h`).

### mutex_lock и fastpath

```c
void __sched mutex_lock(struct mutex *lock)
{
        might_sleep();
        __mutex_fastpath_lock(&lock->count, __mutex_lock_slowpath);
        mutex_set_owner(lock);
}
```

- `might_sleep()` — при `CONFIG_DEBUG_ATOMIC_SLEEP` печатает трассу
  стека, если вызвано в atomic-контексте (отладочный хелпер);
- `__mutex_fastpath_lock` — архитектурная функция
  (`arch/x86/include/asm/mutex_64.h`), пытается уменьшить `count`:

```c
asm_volatile_goto(LOCK_PREFIX "   decl %0\n"
                              "   jns %l[exit]\n"
                              : : "m" (v->counter)
                              : "memory", "cc"
                              : exit);
...
fail_fn(v);
exit:
        return;
```

`LOCK_PREFIX` — префикс `lock;` (атомарность), `decl` уменьшает
счётчик, `jns` прыгает на `exit`, если результат **не отрицательный**
(захват удался). Отрицательный — вызывается `fail_fn`, то есть
`__mutex_lock_slowpath` (midpath/slowpath). Успех — ставим владельца:

```c
static inline void mutex_set_owner(struct mutex *lock)
{
        lock->owner = current;
}
```

### midpath: оптимистичное вращение

`__mutex_lock_slowpath` через `container_of` находит мьютекс и вызывает
`__mutex_lock_common`:

```c
__visible void __sched
__mutex_lock_slowpath(atomic_t *lock_count)
{
        struct mutex *lock = container_of(lock_count,
                                          struct mutex, count);

        __mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0,
                            NULL, _RET_IP_, NULL, 0);
}
```

`__mutex_lock_common` начинается с `preempt_disable()` и пытается
оптимистично вращаться (зависит от `CONFIG_MUTEX_SPIN_ON_OWNER`):

```c
if (mutex_optimistic_spin(lock, ww_ctx, use_ww_ctx)) {
        preempt_enable();
        return 0;
}
```

`mutex_optimistic_spin`:

1. Проверяет, что нет готовых задач с более высоким приоритетом.
2. Занимает место в MCS-очереди: `osq_lock(&lock->osq)` — одновременно
   вращается только один spinner.
3. Крутится в цикле:

```c
while (true) {
    owner = READ_ONCE(lock->owner);

    if (owner && !mutex_spin_on_owner(lock, owner))
        break;

    if (mutex_try_to_acquire(lock)) {
        lock_acquired(&lock->dep_map, ip);

        mutex_set_owner(lock);
        osq_unlock(&lock->osq);
        return true;
    }
}
```

Ждём, пока владелец выполняется на CPU; если появилась более приоритетная
задача — выходим из цикла и спим. Если владелец отпустил мьютекс —
забираем его, выходим из очереди spinner-ов и возвращаем успех.

Если `CONFIG_MUTEX_SPIN_ON_OWNER` выключен, `mutex_optimistic_spin`
просто возвращает `false`.

### slowpath мьютекса

Без успеха вращения функция действует как семафор. Сначала ещё одна
попытка (владелец мог успеть освободить мьютекс):

```c
if (!mutex_is_locked(lock) &&
   (atomic_xchg_acquire(&lock->count, 0) == 1))
      goto skip_wait;
```

Не вышло — добавляемся в очередь:

```c
list_add_tail(&waiter.list, &lock->wait_list);
waiter.task = task;
```

Успех на этом шаге:

```c
skip_wait:
        mutex_set_owner(lock);
        preempt_enable();
        return 0;
```

Иначе — цикл сна (аналогичен семафорному):

```c
for (;;) {

    if (atomic_read(&lock->count) >= 0 &&
        (atomic_xchg_acquire(&lock->count, -1) == 1))
        break;

    if (unlikely(signal_pending_state(state, task))) {
        ret = -EINTR;
        goto err;
    }

    __set_task_state(task, state);

     schedule_preempt_disabled();
}
```

Повторная попытка перед сном гарантирует, что мы получим пробуждение
ровно при разблокировке; по сигналу выходим с `-EINTR`.

### mutex_unlock

```c
void __sched mutex_unlock(struct mutex *lock)
{
    __mutex_fastpath_unlock(&lock->count, __mutex_unlock_slowpath);
}
```

`__mutex_fastpath_unlock` зеркален захвату, но `incl` и `jg`
(прыжок, если результат положителен — мьютекс разблокирован, `count`
вернулся к `1`):

```c
asm_volatile_goto(LOCK_PREFIX "   incl %0\n"
                     "   jg %l[exit]\n"
                     : : "m" (v->counter)
                     : "memory", "cc"
                     : exit);
fail_fn(v);
exit:
        return;
```

Если в очереди есть ожидающие — вызывается `__mutex_unlock_slowpath`
→ `__mutex_unlock_common_slowpath`, который будит первого:

```c
if (!list_empty(&lock->wait_list)) {
    struct mutex_waiter *waiter =
           list_entry(lock->wait_list.next,
                      struct mutex_waiter, list);
                wake_up_process(waiter->task);
}
```

### Дополнительное API мьютексов

- `mutex_lock_interruptible`;
- `mutex_lock_killable`;
- `mutex_trylock`;
- соответствующие версии `unlock`.

Семантика аналогична одноимённым функциям семафоров.

---

## RW-семафоры

### Идея readers/writer

Над данными выполняются две операции — **чтение** и **запись**, причём
чтение обычно чаще. Логично блокировать так, чтобы процессов-читателей
могло быть несколько сразу, пока никто не пишет:

- идёт **запись** — ждут и читатели, и писатели;
- идёт **чтение** — другие читатели не блокируются.

Такой механизм — **readers/writer lock**; в ядре это
`rw_semaphore` — «читательско-писательский» семафор на базе обычного.

### Структура rw_semaphore

`include/linux/rwsem.h` (при выключенном `CONFIG_RWSEM_GENERIC_SPINLOCK`):

```c
struct rw_semaphore {
        long count;
        struct list_head wait_list;
        raw_spinlock_t wait_lock;
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
        struct optimistic_spin_queue osq;
        struct task_struct *owner;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
        struct lockdep_map      dep_map;
#endif
};
```

Для x86_64 (`arch/x86/um/Kconfig`):

```text
config RWSEM_XCHGADD_ALGORITHM
        def_bool 64BIT

config RWSEM_GENERIC_SPINLOCK
        def_bool !RWSEM_XCHGADD_ALGORITHM
```

То есть на 64-битном x86 используется алгоритм **xadd**
(`CONFIG_RWSEM_XCHGADD_ALGORITHM`), а не generic-спинлок.

Первые три поля совпадают с `semaphore` (только `count` — типа `long`).
Поля `osq` и `owner` — как у mutex (optimistic spinning).

### Значения поля count

| Значение | Смысл |
| --- | --- |
| `0x0000000000000000` | свободно, ожидающих нет |
| `0x000000000000000X` | активны X читателей, писателей нет |
| `0xffffffff0000000X` | X читателей + ожидающие; или писатель |
| `0xffffffff00000001` | читатель с ожидающими; или писатель |
| `0xffffffff00000000` | в очереди есть, но никто не активен |
| `0xfffffffe00000001` | писатель активен, есть ожидающие |

Старшее слово — инвертированное число активных писателей, младшее —
число активных читателей. Уточнение неоднозначных значений:

- `0xffffffff0000000X`: активны X читателей и есть ожидающие; либо
  один писатель пытается взять блокировку, ожидающих нет; либо
  писатель активен, ожидающих нет;
- `0xffffffff00000001`: активен один читатель и есть ожидающие; либо
  активен (или пытается стать им) писатель и ожидающих нет.

### Инициализация rwsem

Статически — макросом `DECLARE_RWSEM`:

```c
#define DECLARE_RWSEM(name) \
        struct rw_semaphore name = __RWSEM_INITIALIZER(name)

#define __RWSEM_INITIALIZER(name)              \
{                                              \
        .count = RWSEM_UNLOCKED_VALUE,         \
        .wait_list = LIST_HEAD_INIT((name).wait_list), \
        .wait_lock = __RAW_SPIN_LOCK_UNLOCKED(name.wait_lock) \
         __RWSEM_OPT_INIT(name)                \
}

#define RWSEM_UNLOCKED_VALUE            0x00000000L
```

При `CONFIG_RWSEM_SPIN_ON_OWNER` (включён для x86_64) добавляется:

```c
#define __RWSEM_OPT_INIT(lockname) \
        , .osq = OSQ_LOCK_UNLOCKED, .owner = NULL
```

Динамически — `init_rwsem` → `__init_rwsem`
(`kernel/locking/rwsem-xadd.c` для x86_64; в `kernel/locking/Makefile`
файл выбирается опцией конфигурации):

```c
void __init_rwsem(struct rw_semaphore *sem, const char *name,
                  /* lockdep-параметры опущены */)
{
        sem->count = RWSEM_UNLOCKED_VALUE;
        raw_spin_lock_init(&sem->wait_lock);
        INIT_LIST_HEAD(&sem->wait_list);
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
        sem->owner = NULL;
        osq_lock_init(&sem->osq);
#endif
}
```

### API RW-семафоров

```c
void down_read(struct rw_semaphore *sem);   /* захват на чтение */
int  down_read_trylock(struct rw_semaphore *sem);
void down_write(struct rw_semaphore *sem);  /* захват на запись */
int  down_write_trylock(struct rw_semaphore *sem);
void up_read(struct rw_semaphore *sem);     /* освободить чтение */
void up_write(struct rw_semaphore *sem);    /* освободить запись */
```

### Захват на запись: down_write

`kernel/locking/rwsem.c`:

```c
void __sched down_write(struct rw_semaphore *sem)
{
        might_sleep();
        rwsem_acquire(&sem->dep_map, 0, 0, _RET_IP_);

        LOCK_CONTENDED(sem, __down_write_trylock, __down_write);
        rwsem_set_owner(sem);
}
```

`LOCK_CONTENDED` вызывает `__down_write` → `__down_write_nested`
(`arch/x86/include/asm/rwsem.h`) — inline-ассемблер:

```c
asm volatile("# beginning down_write\n\t"
             LOCK_PREFIX "  xadd      %1,(%2)\n\t"
             "  test " __ASM_SEL(%w1,%k1) ","
                      __ASM_SEL(%w1,%k1) "\n\t"
             "  jz        1f\n"
             "  call call_rwsem_down_write_failed\n"
             "1:\n"
             "# ending down_write"
             : "+m" (sem->count), "=d" (tmp)
             : "a" (sem), "1" (RWSEM_ACTIVE_WRITE_BIAS)
             : "memory", "cc");
```

`xadd` (атомарные «сложение + обмен») прибавляет к `count` смещение:

```c
#define RWSEM_ACTIVE_WRITE_BIAS \
        (RWSEM_WAITING_BIAS + RWSEM_ACTIVE_BIAS)
#define RWSEM_WAITING_BIAS              (-RWSEM_ACTIVE_MASK-1)
#define RWSEM_ACTIVE_BIAS               0x00000001L
```

т.е. `0xffffffff00000001`, и возвращает прежнее значение. Если маска
активных писателей была нуля — блокировка захвачена (`jz 1f`);
иначе — `call_rwsem_down_write_failed` (`arch/x86/lib/rwsem.S`) вызывает
`rwsem_down_write_failed` (`kernel/locking/rwsem-xadd.c`), предварительно
сохранив регистры.

`rwsem_down_write_failed`:

1. Атомарно откатывает write-bias:

   ```c
   count = rwsem_atomic_update(-RWSEM_ACTIVE_WRITE_BIAS, sem);

   static inline long rwsem_atomic_update(long delta,
                                          struct rw_semaphore *sem)
   {
           return delta + xadd(&sem->count, delta);
   }
   ```

2. Пробует optimistic spinning — `rwsem_optimistic_spin`
   (как `mutex_optimistic_spin`).
3. Не вышло — ставит запись в очередь ожидания и помечает счётчик:

   ```c
   waiter.task = current;
   waiter.type = RWSEM_WAITING_FOR_WRITE;

   if (list_empty(&sem->wait_list))
       waiting = false;

   list_add_tail(&waiter.list, &sem->wait_list);
   count = rwsem_atomic_update(RWSEM_WAITING_BIAS, sem);
   ```

4. Уходит в цикл сна до захвата:

   ```c
   while (true) {
       if (rwsem_try_write_lock(count, sem))
           break;
       raw_spin_unlock_irq(&sem->wait_lock);
       do {
           schedule();
           set_current_state(TASK_UNINTERRUPTIBLE);
       } while ((count = sem->count) & RWSEM_ACTIVE_MASK);
       raw_spin_lock_irq(&sem->wait_lock);
   }
   ```

### Захват на чтение: down_read

```c
void __sched down_read(struct rw_semaphore *sem)
{
        might_sleep();
        rwsem_acquire_read(&sem->dep_map, 0, 0, _RET_IP_);

        LOCK_CONTENDED(sem, __down_read_trylock, __down_read);
}
```

`__down_read` — инлайн-ассемблер: увеличивает `count` на единицу
(читатели +1) и проверяет знак:

```c
asm volatile("# beginning down_read\n\t"
             LOCK_PREFIX _ASM_INC "(%1)\n\t"
             "  jns        1f\n"
             "  call call_rwsem_down_read_failed\n"
             "1:\n\t"
             "# ending down_read\n\t"
             : "+m" (sem->count)
             : "a" (sem)
             : "memory", "cc");
```

Знак отрицательный — есть активные писатели, чтение захватить нельзя:
`call_rwsem_down_read_failed` → `rwsem_down_read_failed`:

```c
long adjustment = -RWSEM_ACTIVE_READ_BIAS;

waiter.task = tsk;
waiter.type = RWSEM_WAITING_FOR_READ;

if (list_empty(&sem->wait_list))
    adjustment += RWSEM_WAITING_BIAS;
list_add_tail(&waiter.list, &sem->wait_list);

count = rwsem_atomic_update(adjustment, sem);
```

Если активных блокировок нет и мы первы в очереди — присоединяемся к
активным читателям, иначе спим.

### Освобождение: up_write и up_read

```c
void up_write(struct rw_semaphore *sem)
{
        rwsem_release(&sem->dep_map, 1, _RET_IP_);

        rwsem_clear_owner(sem);
        __up_write(sem);
}

static inline void rwsem_clear_owner(struct rw_semaphore *sem)
{
        sem->owner = NULL;
}
```

`__up_write` (архитектурный) делает то же, что `__down_write`, но
наоборот: вычитает `RWSEM_ACTIVE_WRITE_BIAS` и проверяет знак прежнего
значения:

- прежнее значение **не отрицательное** — писатель освободил
  блокировку, её может взять кто угодно;
- значение отрицательное — в очереди есть писатели: вызывается
  `call_rwsem_wake` → `rwsem_wake`, который сначала проверяет
  наличие spinner-а (тот просто забирает блокировку), будит
  писателя сверху очереди или всех читателей.

`up_read` похож: вместо вычитания write-bias вычитается `1`
(читатели хранятся в младшем слове), затем проверка знака и, если
`count` отрицателен — `rwsem_wake`.

---

## Seqlock (sequential lock)

### Идея seqlock

У readers/writer lock есть проблема **writer starvation**: писатель не
получит блокировку, пока есть хоть один активный читатель, а при
высокой конкуренции это может длиться долго.

**Seqlock** (появился в Linux 2.6.x) даёт **быстрый безблокировочный
доступ** к общим ресурсам. В основе — спинлок и **счётчик событий**:

- **писатель**: захватывает спинлок, увеличивает счётчик, пишет
  данные, снова увеличивает счётчик и отпускает спинлок;
- **читатель**: свободно читает, но обязан проверить конфликты:
  берёт значение счётчика **до** входа в критическую секцию и
  сравнивает с значением **после** выхода. Совпали — писателей не
  было; не совпали — чтение нужно повторить.

```c
unsigned int seq_counter_value;

do {
    seq_counter_value = get_seq_counter_val(&the_lock);
    /* работа с данными */
} while (__retry__);
```

(`get_seq_counter_val` и `__retry__` — заглушки, настоящий API ниже.)
Seqlock хорош, когда защищаемые ресурсы малы и просты, а запись
бывает редко и выполняется быстро.

### Структура seqlock_t

`include/linux/seqlock.h`:

```c
typedef struct {
        struct seqcount seqcount;
        spinlock_t lock;
} seqlock_t;

typedef struct seqcount {
        unsigned sequence;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
        struct lockdep_map dep_map;
#endif
} seqcount_t;
```

Счётчик + спинлок, защищающий данные от других писателей.

### Инициализация seqlock

Статически:

```c
#define DEFINE_SEQLOCK(x) \
                seqlock_t x = __SEQLOCK_UNLOCKED(x)

#define __SEQLOCK_UNLOCKED(lockname)                 \
        {                                             \
                .seqcount = SEQCNT_ZERO(lockname),    \
                .lock = __SPIN_LOCK_UNLOCKED(lockname) \
        }

#define SEQCNT_ZERO(lockname) \
        { .sequence = 0, SEQCOUNT_DEP_MAP_INIT(lockname) }
```

`SEQCNT_DEP_MAP_INIT` относится к lockdep — опущен. Счётчик обнуляется,
спинлок — в `unlocked`.

Динамически:

```c
#define seqlock_init(x)                 \
        do {                            \
                seqcount_init(&(x)->seqcount);     \
                spin_lock_init(&(x)->lock);        \
        } while (0)

static inline void __seqcount_init(seqcount_t *s, const char *name,
                                   /* lockdep-параметры опущены */)
{
        s->sequence = 0;
}
```

### API seqlock

```c
static inline unsigned read_seqbegin(const seqlock_t *sl);
static inline unsigned read_seqretry(const seqlock_t *sl,
                                     unsigned start);
static inline void write_seqlock(seqlock_t *sl);
static inline void write_sequnlock(seqlock_t *sl);
static inline void write_seqlock_irq(seqlock_t *sl);
static inline void write_sequnlock_irq(seqlock_t *sl);
static inline void read_seqlock_excl(seqlock_t *sl);
static inline void read_sequnlock_excl(seqlock_t *sl);
```

Читателей два типа: **без блокировки** (не мешают писателю —
`read_seqbegin`/`read_seqretry`) и **блокирующие**
(`read_seqlock_excl` — берут сам спинлок, писатель ждёт).

### Чтение: read_seqbegin и read_seqretry

```c
static inline unsigned read_seqbegin(const seqlock_t *sl)
{
        return read_seqcount_begin(&sl->seqcount);
}

static inline unsigned raw_read_seqcount(const seqcount_t *s)
{
        unsigned ret = READ_ONCE(s->sequence);
        smp_rmb();
        return ret;
}
```

Читаем счётчик (с **барьером чтения** `smp_rmb`, чтобы чтение данных
не «перепрыгнуло» чтение счётчика). На выходе из критической секции:

```c
static inline unsigned read_seqretry(const seqlock_t *sl,
                                     unsigned start)
{
        return read_seqcount_retry(&sl->seqcount, start);
}

static inline int __read_seqcount_retry(const seqcount_t *s,
                                        unsigned start)
{
        return unlikely(s->sequence != start);
}
```

Если начальное значение счётчика было **нечётным**, писатель как раз
находился в середине обновления — данные могли быть в
противоречивом состоянии, чтение повторяется.

### Пример: get_jiffies_64

Типичный паттерн чтения `jiffies` на x86_64:

```c
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

Тело `do/while` выполняется минимум один раз; если счётчик
изменился — читаем заново.

Блокирующий читатель (тип 2) просто берёт и отпускает спинлок:

```c
static inline void read_seqlock_excl(seqlock_t *sl)
{
        spin_lock(&sl->lock);
}

static inline void read_sequnlock_excl(seqlock_t *sl)
{
        spin_unlock(&sl->lock);
}
```

### Запись: write_seqlock

```c
static inline void write_seqlock(seqlock_t *sl)
{
        spin_lock(&sl->lock);
        write_seqcount_begin(&sl->seqcount);
}

static inline void raw_write_seqcount_begin(seqcount_t *s)
{
        s->sequence++;
        smp_wmb();
}
```

Спинлок исключает других писателей, счётчик увеличивается (делает его
нечётным — «обновление идёт») с **барьером записи** `smp_wmb`.

Освобождение:

```c
static inline void write_sequnlock(seqlock_t *sl)
{
        write_seqcount_end(&sl->seqcount);
        spin_unlock(&sl->lock);
}

static inline void raw_write_seqcount_end(seqcount_t *s)
{
        smp_wmb();
        s->sequence++;
}
```

Счётчик снова чётен, спинлок отпущен. Варианты для обработчиков
прерываний отличаются только типом захвата спинлока:

```c
static inline void write_seqlock_irq(seqlock_t *sl)
{
        spin_lock_irq(&sl->lock);
        write_seqcount_begin(&sl->seqcount);
}

static inline void write_sequnlock_irq(seqlock_t *sl)
{
        write_seqcount_end(&sl->seqcount);
        spin_unlock_irq(&sl->lock);
}
```

Аналогично `write_seqlock_irqsave` / `write_sequnlock_irqrestore`
(`spin_lock_irqsave`/`spin_unlock_irqsave`) — для IRQ-контекста.

---

## Шпаргалка

### Когда какой примитив

| Примитив | Когда использовать |
| --- | --- |
| Спинлок | Очень короткая критическая секция, нельзя спать |
| Queued spinlock | То же, но per-cpu очередь вместо общего адреса |
| Семафор | Долгая критическая секция, можно спать, учёт ресурсов |
| Mutex | Долгая секция, строго один владелец, разблокирует только он |
| RW-семафор | Много читателей редко + редкая запись |
| Seqlock | Малые данные, быстрая редкая запись, читатели без блокировок |

### Инициализация

| Примитив | Статически | Динамически |
| --- | --- | --- |
| Спинлок | `__SPIN_LOCK_UNLOCKED` | `spin_lock_init` |
| Семафор | `DEFINE_SEMAPHORE` | `sema_init` |
| Mutex | `DEFINE_MUTEX` | `mutex_init` |
| RW-семафор | `DECLARE_RWSEM` | `init_rwsem` |
| Seqlock | `DEFINE_SEQLOCK` | `seqlock_init` |

### Основные вызовы

| Захват | Освобождение | Примечание |
| --- | --- | --- |
| `spin_lock` | `spin_unlock` | `_bh`, `_irq`, `_irqsave`-варианты |
| `down` | `up` | `down_trylock`, `down_timeout`, `down_interruptible` |
| `mutex_lock` | `mutex_unlock` | fast/mid/slow-path |
| `down_read` | `up_read` | чтение RW-семафора |
| `down_write` | `up_write` | запись RW-семафора |
| `read_seqbegin` | `read_seqretry` | читатель seqlock, проверка на выходе |
| `write_seqlock` | `write_sequnlock` | писатель seqlock |

### Ключевые факты

- `spinlock_t` → `raw_spinlock_t` → `arch_spinlock_t` = `qspinlock`
  на x86_64.
- Поле `qspinlock.val` (32 бита): `locked` (0–7), `pending` (8),
  индекс MCS (16–17), номер CPU — хвост (18–31).
- MCS-лок: вращаемся на **своей** per-cpu копии (`qnodes[MAX_NODES]`,
  4 контекста: задача, аппаратное и программное прерывление, NMI).
- `semaphore.count > 0` — захват; ожидающие спят в `wait_list`,
  пробуждаются функцией `up` → `__up` → `wake_up_process`.
- `mutex.count`: `1` — свободен, `0` — занят, отрицательный —
  занят с ожидающими.
- Три пути мьютекса: fastpath (`decl`/`jns`), midpath (optimitic
  spinning через `osq`), slowpath (сон в `wait_list`).
- `rw_semaphore.count` (64 бита): старшее слово — инвертированные
  активные писатели, младшее — читатели; захват через `xadd`.
- Seqlock: счётчик нечётен — писатель в критической секции;
  `smp_rmb`/`smp_wmb` гарантируют порядок; читатель сверяет счётчик
  до и после чтения.
