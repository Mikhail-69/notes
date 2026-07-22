# 📘 Multi-stage builds в Docker

> Полное руководство по многоэтапной сборке Docker-образов.  
> Перевод и структурирование официальной документации.

---

## 🧠 Что такое Multi-stage builds

**Multi-stage builds** (многоэтапная сборка) — это подход, который позволяет использовать несколько инструкций `FROM` в одном Dockerfile. Каждая инструкция `FROM` начинает новый этап сборки. Вы можете выборочно копировать артефакты из одного этапа в другой, оставляя за бортом всё, что не нужно в финальном образе.

### Зачем это нужно

- **Маленький финальный образ** — в нём остаётся только приложение, без компиляторов, SDK и промежуточных файлов.
- **Читаемость Dockerfile** — всё в одном файле, без отдельных скриптов сборки.
- **Безопасность** — меньше уязвимостей, так как в образе нет лишних инструментов.

---

## 🔧 Базовый пример

```dockerfile
# syntax=docker/dockerfile:1

FROM golang:1.25
WORKDIR /src
COPY <<EOF ./main.go
package main

import "fmt"

func main() {
    fmt.Println("hello, world")
}
EOF
RUN go build -o /bin/hello ./main.go

FROM scratch
COPY --from=0 /bin/hello /bin/hello
CMD ["/bin/hello"]
```
- Первый этап `(FROM golang:1.25)` — сборка бинарника.

- Второй этап `(FROM scratch)` — чистый образ, в который копируется только собранный бинарник.

Результат
Финальный образ содержит только бинарник. Никаких инструментов сборки, компиляторов или исходников.

## 🏷️ Как работает нумерация этапов
По умолчанию этапы нумеруются с `0` (первый `FROM`).
`COPY --from=0` — копирует из первого этапа.
---
## 📛 Именование этапов
Чтобы не запутаться в индексах, можно давать этапам имена через `AS`:
```dockerfile
# syntax=docker/dockerfile:1

FROM golang:1.25 AS build
WORKDIR /src
COPY <<EOF /src/main.go
package main

import "fmt"

func main() {
    fmt.Println("hello, world")
}
EOF
RUN go build -o /bin/hello ./main.go

FROM scratch
COPY --from=build /bin/hello /bin/hello
CMD ["/bin/hello"]
```
Теперь можно ссылаться на этап по имени: `COPY --from=build`.

## 🎯 Сборка до конкретного этапа (`--target`)
Можно остановить сборку на определённом этапе:
```bash
docker build --target build -t hello .
```
### Когда это полезно
- Отладка — собрать этап с отладочными символами.
- Тестирование — собрать этап с тестовыми данными.
- Production — собрать только финальный этап.
## 📦 Использование внешнего образа как этапа
Можно копировать файлы из любого образа, не только из предыдущих этапов:
```dockerfile
COPY --from=nginx:latest /etc/nginx/nginx.conf /nginx.conf
```
Docker скачает образ `nginx:latest` и скопирует оттуда нужный файл.
## 🔁 Использование предыдущего этапа как базы для нового
Можно создать несколько этапов от одной базы:
```dockerfile
# syntax=docker/dockerfile:1

FROM alpine:latest AS builder
RUN apk --no-cache add build-base

FROM builder AS build1
COPY source1.cpp source.cpp
RUN g++ -o /binary source.cpp

FROM builder AS build2
COPY source2.cpp source.cpp
RUN g++ -o /binary source.cpp
```
Это удобно, когда нужно собрать несколько вариантов из одного базового окружения.
## ⚡ Разница между legacy builder и BuildKit
### Legacy builder (старый)
- Обрабатывает все этапы, даже если они не нужны для целевого этапа.
- Медленнее и менее оптимален.
### BuildKit (новый, рекомендуемый)
- Строит только те этапы, от которых зависит целевой этап.
- Быстрее и эффективнее.
Пример:
```dockerfile
# syntax=docker/dockerfile:1
FROM ubuntu AS base
RUN echo "base"

FROM base AS stage1
RUN echo "stage1"

FROM base AS stage2
RUN echo "stage2"
```

С BuildKit (цель `stage2`):
```text
$ DOCKER_BUILDKIT=1 docker build --no-cache -f Dockerfile --target stage2 .
[+] Building 0.4s (7/7) FINISHED
=> CACHED [base 1/2] FROM docker.io/library/ubuntu
=> [base 2/2] RUN echo "base"
=> [stage2 1/1] RUN echo "stage2"
```

**stage1** пропущен, так как не нужен для stage2.

**Без BuildKit** (цель stage2):
```text
$ DOCKER_BUILDKIT=0 docker build --no-cache -f Dockerfile --target stage2 .
Step 4/6 : RUN echo "stage1"
stage1
```

## 🧠 Итог: когда использовать Multi-stage

| Сценарий | Решение |
|-----------|-----------|
| Нужен маленький финальный образ  | ✅ Multi-stage |
| Хочешь убрать компиляторы и SDK из продакшена | ✅ Multi-stage  |
| Нужно собрать несколько вариантов из одной базы  | ✅ Multi-stage с именами этапов |
| Хочешь быстро отлаживать отдельные этапа | ✅ --target + BuildKit  |
| Используешь Docker < 20.10 | ⚠️ Включи BuildKit вручную  |

## 📌 Полезные команды
```bash
# Собрать с BuildKit (включён по умолчанию в новых версиях)
docker build -t hello .

# Собрать до конкретного этапа
docker build --target build -t hello .

# Отключить BuildKit (не рекомендуется)
DOCKER_BUILDKIT=0 docker build -t hello .
```
