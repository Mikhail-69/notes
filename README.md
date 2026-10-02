# 📚 notes

> Личное хранилище знаний: конспекты, книги, чек-листы, инструменты.  
> Репозиторий живой — постоянно дополняется и рефакторится.

---

## 🧭 О репозитории

Здесь лежит всё, что я и другие контрибьюторы собираем по ходу учёбы и работы:

- **конспекты** по программированию, сетям, Linux, DevOps;
- **книги** по темам (программирование, психология, бизнес);
- **шпаргалки** по инструментам (Git, Markdown, Regex, Docker);
- **чек-листы** по безопасности и администрированию;
- **подборки** полезных open-source проектов.

С авторами репозитория можно ознакомиться **[тут](AUTHORS)**.

---

## 👥 Для контрибьюторов

Если хочешь заливать сюда свои материалы или править существующие — **сначала прочитай [руководство для участников](шаблоны/attention/GuideForWork.md)**.

В нём описаны:

- как раскатить окружение;
- в какой ветке работать;
- как называть коммиты (Conventional Commits);
- как создавать Pull Request.

---

## 📂 Структура

```text
notes/
├── README.md                       # Этот файл
├── AGENTS.md                       # Инструкции для AI-агентов
├── AUTHORS                         # Авторы репозитория
├── .github/
│   ├── workflows/                  # GitHub Actions (CI/CD)
│   └── CODEOWNERS                  # Защита файлов
├── .gitignore
│
├── шаблоны/                        # Болванки для заметок
│   ├── Оформление/
│   │   ├── Conversial Commits.md   # Правила коммитов
│   │   └── Markdown.md             # Шпаргалка по Markdown
│   └── attention/
│       └── GuideForWork.md         # Инструкция для контрибьюторов
│
├── github_project/                 # База ссылок на open-source проекты
│   ├── learning.md
│   ├── selfhosted.md
│   ├── devops-tools.md
│   ├── security-privacy.md
│   ├── productivity.md
│   ├── media-creators.md
│   └── fun.md
│
├── S.E.E.D.1956/                   # Созданный мною квест для программистов
│
├── books/                          # Книги по темам
│   ├── Программирование/
│   ├── Психология/
│   └── Other/
│
├── boardgames/                     # Настольные игры
│
├── саморазвитие/                   # Курсы, статьи, пет-проекты
│   ├── Ivanov_M_V/                 # Личные проекты (C++, DevOps)
│   │   └── DevOps/
│   ├── hacker101/                  # Кибербезопасность
│   ├── полезное/                   # Шпаргалки (Regex, IT-tools)
│   └── viruses/                    # Материалы по вирусам
│
└── images/                         # Все картинки для конспектов
    ├── diagrams/                   # Схемы и диаграммы
    ├── screenshots/                # Скриншоты
    └── memes/                      # Мемы
```

## 🛠️ Стек

| Компонент | Назначение |
|-----------|------------|
| **Markdown** | Форматирование конспектов |
| **Git + GitHub** | Версионный контроль и синхронизация |
| **VS Code** | Редактор |
| **Git Graph** | Визуализация веток |
| **GitHub Actions** | CI/CD (проверки PR) |

---

## 🚀 Быстрый старт

```bash
# 1. Клонировать репозиторий
git clone https://github.com/Mikhail-69/notes.git
cd notes

# 2. Переключиться на рабочую ветку
git checkout dev

# 3. Создать свою ветку
git checkout -b твоё_имя/название_задачи

# 4. Работать, коммитить, пушить
git add .
git commit -m "feat(scope): описание"
git push -u origin твоё_имя/название_задачи
```

Дальше — создать Pull Request в dev.

## 📋 Правила для PR

- Все изменения — только через **Pull Request** в `dev`.
- Название ветки: `username/feature-name` или `username/fix-description`.
- Коммиты — по **[Conventional Commits](шаблоны/Оформление/Conversial%20Commits.md)**.
- Перед PR — убедись, что ветка актуальна относительно `dev`.

---

## 📫 Контакты

| Платформа | Ссылка |
|-----------|--------|
| **Telegram** | [@iamivanovmikhail](https://t.me/iamivanovmikhail) |
| **GitHub** | [Mikhail-69](https://github.com/Mikhail-69) |
| **Email** | [awesome.myk44i10@yandex.ru](mailto:awesome.myk44i10@yandex.ru) |
| **Телефон** | 8-931-311-51-33 |
| **Все репозитории** | [Mikhail-69?tab=repositories](https://github.com/Mikhail-69?tab=repositories) |

---

## 📜 Лицензия

Весь контент — авторский. Используй с умом.
