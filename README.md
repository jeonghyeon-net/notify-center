# notify-center

Xfce(X11)용 알림 서버입니다. 팝업, 그리고 패널 종 버튼으로 여는 모바일식 알림 목록을 한 프로그램이 처리합니다.
xfce4-notifyd나 dunst를 대신해 `org.freedesktop.Notifications`를 직접 받습니다.

## 왜 만들었나

보통 알림 서버는 팝업이 사라지면 앱에 "알림이 닫혔다"(`NotificationClosed`)는 신호를 보냅니다.
그러면 Slack 같은 앱은 그 알림이 무엇이었는지 잊어버려서, 나중에 알림 기록에서 눌러도 해당 스레드로 이동하지 않습니다.

notify-center는 팝업이 사라져도 알림을 닫지 않고 목록에 **살아 있는 채로** 둡니다(`persistence`, GNOME이나 휴대폰과 같은 방식).
그래서 목록에서 눌러도 팝업을 누른 것과 똑같이 앱의 동작(스레드 이동 등)이 실행됩니다.

## 기능

- **팝업**: 화면 오른쪽 아래, 최대 3개까지 쌓입니다. 넘치면 가장 오래된 팝업만 내려가고 알림은 목록에 남습니다.
  - 왼쪽 클릭: 알림 동작 실행 후 해결
  - 오른쪽 클릭: 떠 있는 팝업을 모두 내림 (목록에는 남음)
- **알림 목록**: 해결하지 않은 알림을 최신순으로 보여줍니다.
  - 카드 클릭: 알림 동작 실행 후 해결. 동작이 없거나 앱이 창을 띄우지 못하면 그 앱 창을 앞으로 가져옵니다.
  - ✕: 해결(목록에서 지우기) / 모두 지우기
  - Esc, 바깥 클릭, 다시 토글하면 닫힘
  - 목록이 열려 있는 동안 새 알림은 팝업 없이 목록에 바로 추가됩니다.
- **패널 종 버튼**: 해결 안 된 알림 개수를 빨간 배지로 표시합니다(xfce4-genmon-plugin 사용).
- 앱이 알림을 거둬 가면(`CloseNotification`, 예: 다른 기기에서 읽음) 목록에서도 빠집니다.
- 볼륨·밝기처럼 계속 갱신되는 일시적 알림(`transient`, 진행률 `value`, `x-canonical-private-synchronous` 등)은 팝업으로만 띄우고 목록에 남기지 않습니다.

## 요구 사항

- X11 세션 (Xfce에서 사용·테스트함. Wayland는 지원하지 않음)
- GTK 3, libX11, pkg-config, C 컴파일러, make
- 패널 배지를 쓰려면 xfce4-genmon-plugin
- `busctl` (systemd)

```sh
# Arch 계열
sudo pacman -S --needed base-devel gtk3 libx11 xfce4-genmon-plugin
# Debian/Ubuntu 계열
sudo apt install build-essential pkg-config libgtk-3-dev libx11-dev xfce4-genmon-plugin
```

## 설치

```sh
make
make install            # 기본 위치: ~/.local (PREFIX=... 로 변경 가능)
```

`make install`은 다음을 설치합니다.

| 파일 | 역할 |
|---|---|
| `~/.local/bin/notify-center` | 알림 서버 |
| `~/.local/bin/notify-badge` | 패널 종 버튼 배지 스크립트 |
| `~/.local/share/dbus-1/services/org.freedesktop.Notifications.service` | 알림이 notify-center로 오게 함 (꺼져 있으면 자동 실행) |
| `~/.config/autostart/notify-center.desktop` | 로그인 시 실행 |
| `~/.config/autostart/xfce4-notifyd.desktop` | xfce4-notifyd 자동 시작 끄기 |

설치 후 실행 중인 기존 알림 서버를 끄고 notify-center를 시작합니다.

```sh
busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig
systemctl --user stop xfce4-notifyd.service dunst.service 2>/dev/null
pkill -x xfce4-notifyd; pkill -x dunst
setsid -f ~/.local/bin/notify-center
```

dunst 등 다른 알림 서버 패키지가 설치되어 있으면 지우는 것을 권장합니다.

### 패널 종 버튼

1. 패널 우클릭 → 패널 → 새 항목 추가 → **일반 모니터(Generic Monitor)**
2. 그 항목의 속성에서
   - 명령: `~/.local/bin/notify-badge` (전체 경로로 입력)
   - 레이블: 끄기
   - 주기: 60초 (안전장치용. 평소에는 notify-center가 개수가 바뀔 때마다 바로 갱신 신호를 보냄)

notify-center는 갱신 신호를 **모든** genmon 플러그인에 보냅니다. genmon을 다른 용도로도 쓰고 있다면 알림 개수가 바뀔 때 그 플러그인들도 함께 새로 고쳐집니다.

### 단축키 (선택)

설정 → 키보드 → 응용 프로그램 바로 가기에 다음 명령을 원하는 키(예: Super+N)로 등록합니다.

```sh
busctl --user call org.freedesktop.Notifications /org/freedesktop/Notifications local.notify.Center Toggle
```

## D-Bus 인터페이스

표준 `org.freedesktop.Notifications` (사양 1.2) 외에, 같은 객체(`/org/freedesktop/Notifications`)에 다음을 제공합니다.

| `local.notify.Center` | |
|---|---|
| `Toggle()` | 알림 목록 열기/닫기 |
| `Count() → u` | 해결 안 된 알림 개수 |

`GetCapabilities`: `actions`, `body`, `body-markup`, `icon-static`, `persistence`

## 고치기

팝업 개수, 크기, 기본 표시 시간(10초), 여백은 `notify-center.c` 맨 위 `#define`에, 색과 간격은 그 아래 `CSS`에 모여 있습니다. 기본 색은 어두운 GTK 테마 기준입니다.
고친 뒤 `make install` 후 다시 실행합니다.

```sh
make install && pkill -x notify-center; setsid -f ~/.local/bin/notify-center
```

## 제한 사항

- 알림 안의 추가 버튼("답장" 등)은 표시하지 않습니다. 클릭 시 기본 동작(`default`)만 실행합니다.
- 진행률 막대, 알림 소리, 본문의 링크·이미지 태그는 지원하지 않습니다(링크는 일반 글자로 표시).
- 알림 목록은 메모리에만 있어서 로그아웃·재부팅하면 사라집니다.
- X11 전용입니다.

## 제거

```sh
make uninstall
busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig
pkill -x notify-center
```

이후 xfce4-notifyd는 다음 알림이 올 때 자동으로 다시 실행되고, 다음 로그인부터는 평소처럼 시작됩니다.
패널의 일반 모니터 항목은 직접 지웁니다.
