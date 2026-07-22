# 🖥️ Self-hosted — всё для своего сервера

> Коллекция сервисов, которые можно поднять на своём железе.

---

## 📦 Платформы и панели управления

| Проект | Описание |
| -------- | ---------- |
| **[Coolify](https://github.com/coollabsio/coolify)** | Деплой приложений, баз данных и сервисов на своём сервере без DevOps-экспертизы. Альтернатива Heroku и Vercel. |
| **[Supabase](https://github.com/supabase/supabase)** | Полноценная альтернатива Firebase на PostgreSQL. Из коробки: авторизация, хранилище, real-time подписки и REST API. |
| **[Awesome-Selfhosted](https://github.com/awesome-selfhosted/awesome-selfhosted)** | Огромный каталог self-hosted альтернатив облачным сервисам. Тысячи решений с описаниями и лицензиями. |
| **[Meshery](https://github.com/meshery/meshery)** | Self-hosted платформа для управления Kubernetes-инфраструктурой и облачными сервисами. GitOps-подход, визуальный редактор, 380+ интеграций. |

---

## ☁️ Облачные хранилища и файлы

| Проект | Описание |
| -------- | ---------- |
| **[Nextcloud](https://github.com/nextcloud/server)** | Self-hosted облачное хранилище. Альтернатива Google Drive и Dropbox. Синхронизация, офисный редактор, календарь и контакты. |
| **[Cloudreve](https://github.com/cloudreve/cloudreve)** | Система управления файлами с поддержкой нескольких провайдеров: локальный диск, OneDrive, S3 и другие. Есть WebDAV и ссылки с сроком действия. |
| **[ownCloud](https://github.com/owncloud/core)** | Серверная часть ownCloud 10 (Classic). Хранение, синхронизация и обмен файлами. Поддерживает WebDAV, CalDAV, CardDAV, шифрование и REST API. |
| **[Seafile](https://github.com/haiwen/seafile)** | Self-hosted облачное хранилище с библиотеками, клиентским шифрованием и блочной синхронизацией. Поддерживает выборочную синхронизацию папок. |
| **[FileBrowser](https://github.com/filebrowser/filebrowser)** | Веб-интерфейс для управления файлами на сервере. Загрузка, удаление, редактирование через браузер. Единый бинарный файл. |

---

## 🎬 Медиасерверы

| Проект | Описание |
|--------|----------|
| **[Jellyfin](https://github.com/jellyfin/jellyfin)** | Self-hosted медиасервер для фильмов, сериалов и музыки. Красивый интерфейс, стриминг на любое устройство. Альтернатива Plex без подписки. |

---

## 📧 Почтовые серверы

| Проект | Описание |
| -------- | ---------- |
| **[Mail-in-a-Box](https://github.com/mail-in-a-box/mailinabox)** | Полноценный почтовый сервер на Ubuntu 22.04 за один шаг. Postfix, Dovecot, Roundcube, антиспам, DNS с SPF/DKIM/DMARC. |
| **[Mailcow](https://github.com/mailcow/mailcow-dockerized)** | Почтовый сервер в Docker. Postfix, Dovecot, Rspamd, SOGO и веб-интерфейс. Всё в одном контейнере. |
| **[Docker Mailserver](https://github.com/docker-mailserver/docker-mailserver)** | Почтовый сервер в одном Docker-контейнере. Postfix, Dovecot, Rspamd, ClamAV, OpenDKIM, Fail2ban. Без базы данных — только конфиги. |

---

## 🌐 DNS и сеть

| Проект | Описание |
| -------- | ---------- |
| **[AdGuard Home](https://github.com/AdguardTeam/AdGuardHome)** | DNS-сервер с блокировкой рекламы и трекеров на уровне всей сети. Ставится на роутер или Raspberry Pi. |
| **[Technitium DNS Server](https://github.com/TechnitiumSoftware/DnsServer)** | Self-hosted DNS-сервер с веб-интерфейсом. Блокировка рекламы, поддержка DNS-over-HTTPS и DNS-over-TLS. Альтернатива Pi-hole. |
| **[ZeroTier](https://github.com/zerotier/ZeroTierOne)** | Виртуальный Ethernet поверх интернета. Устройства оказываются в одной «локальной» сети, даже разбросанные по разным площадкам. |

---

## 🔒 VPN и удалённый доступ

| Проект | Описание |
|--------|----------|
| **[RustDesk](https://github.com/rustdesk/rustdesk)** | Удалённый рабочий стол с открытым исходным кодом. Альтернатива TeamViewer. Можно использовать свои rendezvous/relay-серверы. |

---

## 📊 Мониторинг и аналитика

| Проект | Описание |
|--------|----------|
| **[Plausible](https://github.com/plausible/analytics)** | Замена Google Analytics без слежки за пользователями. Лёгкий и приватный. |
| **[PostHog](https://github.com/PostHog/posthog)** | Аналитика пользователей, запись сессий, A/B-тесты. Альтернатива Mixpanel на своём сервере. |


## 🔧 Self-hosted утилиты

| Проект | Описание |
|--------|----------|
| **[Ghost Provider](https://github.com/GHOST-PROVIDER/GHOST-PROVIDER)** | Разворачивайте self-hosted сервисы без лишней настройки. Поддерживает SearXNG, Open WebUI, Memos и VERT. |
| **[Traefik](https://github.com/traefik/traefik)** | Обратный прокси-сервер и балансировщик нагрузки для микросервисов. Автоматическая генерация маршрутов через API оркестратора. |
| **[Pi-hole](https://github.com/pi-hole/pi-hole)** | DNS sinkhole для блокировки рекламы и трекеров на уровне всей сети. Не требует установки клиентов. |
| **[Proxmox Mail Gateway](https://github.com/proxmox-mail-gateway/pmg)** | Защищённый прокси-шлюз для фильтрации спама и вирусов в почтовом трафике. |
| **[Ceph](https://github.com/ceph/ceph)** | Распределённая система хранения данных с открытым исходным кодом. Поддерживает объектное, блочное и файловое хранение. |
| **[RedisInsight](https://github.com/RedisInsight/RedisInsight)** | Графический интерфейс для Redis, позволяющий управлять базами данных и данными. |