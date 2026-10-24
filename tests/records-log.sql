-- Rows copied from this laptop's records on 2026-10-23 at 00:01 (tests/test_core.cpp, test_records_log).
-- Crash and unit-failure details keep their first line; package details are cut at 1200 characters.
INSERT INTO log(t, category, title, detail) VALUES(1788129142.0, 'software', 'Installed forkstat', 'Command: pacman --upgrade --noconfirm -- /home/user/.cache/paru/clone/forkstat/forkstat-0.03.02-1-x86_64.pkg.tar.zst
Installed:
  forkstat (0.03.02-1)
');
INSERT INTO log(t, category, title, detail) VALUES(1788129810.0, 'software', 'Installed 21 packages; upgraded 824 packages', 'Command: pacman -Syu --ignore audit --needed bc sysstat websocat mtr iperf3 rtl-sdr rtl_433 uv hyperfine
Installed:
  bc (1.08.2-1.1)
  sysstat (12.7.9-1.1)
  websocat (1.14.1-2)
  mtr (0.96-1.1)
  lksctp-tools (1.0.21-1.3)
  iperf3 (3.21-1.1)
  rtl-sdr (1:2.0.3-1.1)
  soapysdr (0.8.1-7.1)
  rtl_433 (25.12-1.1)
  uv (0.12.7-1.1)
  hyperfine (1.20.0-1.1)
  libexif (0.6.26-1.1)
  libcgif (0.5.3-1.1)
  libvips (8.18.5-1)
  litehtml (0.10-1.1)
  librustls (0.15.3-1.1)
  haskell-lua-arbitrary (1.0.1.2-43)
  haskell-tasty-lua (1.1.2-5)
  haskell-tasty-hunit (0.10.2-300)
  haskell-asciidoc (0.1.0.4-5)
  haskell-skylighting-format-typst (0.1-6)
Upgraded:
  iana-etc (20260530-1 -> 20260617-1)
  glibc (2.43+r37+gfdf10644d6ee-2 -> 2.44+r24+g16be1518495f-1)
  libgcc (16.1.1+r346+g4e03491b401d-2 -> 16.2.1+r23+gd564253eb6c8-1)
  libstdc++ (16.1.1+r346+g4e03491b401d-2 -> 16.2.1+r23+gd564253eb6c8-1)
  libasan (16.1.1+r346+g4e03491b401d-2 -> 16.2.1+r23+gd564253eb6c8-1)
  libatomic (16.1.1+r346+g4e03491b401d-2 -> 16.2.1+r23+gd564253eb6c8-1)
  libgfortran (16.1.1+r346+g4e03491b401d-2 -> 16.2.1+r23+gd564253eb6c8-1)
  libgomp (16.1.1+r346+g4e03491b401d-2 -> 16.2.1+r23+gd564253eb6c8-1)
  libhwasan (16.1');
INSERT INTO log(t, category, title, detail) VALUES(1789937851.0, 'software', 'Installed tailscale', 'Command: pacman -S tailscale
Installed:
  tailscale (1.102.4-1.1)
');
INSERT INTO log(t, category, title, detail) VALUES(1790299840.0, 'software', 'Installed 67 packages', 'Command: pacman -S --needed wireshark-qt tcpdump metasploit hashcat john aircrack-ng sqlmap nikto gobuster masscan gitleaks testssl.sh lynis trivy ghidra binwalk rizin sigrok-cli pulseview mitmproxy
Installed:
  bcg729 (1.1.1-2.1)
  wireshark-cli (4.7.3-1)
  wireshark-qt (4.7.3-1)
  postgresql-libs (18.6-1.1)
  rubygems (3.6.9-1)
  ruby (3.4.10-1)
  ruby-bundler (4.0.3-1)
  ruby-erb (4.0.4.1-1.1)
  metasploit (6.5.0-1.1)
  hashcat (1:7.1.2-2.1)
  john (1.9.0.jumbo1-13)
  net-tools (2.10-3.1)
  aircrack-ng (1.7-7.1)
  sqlmap (1.10.7-1)
  perl-json (4.11-1)
  perl-net-ssleay (1.96-1.1)
  nikto (2.6.0-2)
  gobuster (3.8.2-1.1)
  masscan (1.3.2-2.2)
  gitleaks (8.30.1-1.1)
  testssl.sh (3.2.4-1)
  lynis (3.1.7-1)
  trivy (0.73.0-1.1)
  ghidra (12.1.2-2.1)
  binwalk (3.1.0-1.1)
  capstone (5.0.9-1.1)
  tree-sitter (0.26.9-1.1)
  rizin (0.8.2-2.1)
  libsigc++ (2.12.2-1.1)
  glibmm (2.66.10-1.1)
  libieee1284 (0.2.11-19.1)
  libserialport (0.1.2-1)
  libsigrok (0.5.2-26.1)
  libsigrokdecode (0.5.3-10.4)
  sigrok-cli (0.7.2-3.1)
  qt5-svg (5.15.19+kde+r5-1.1)
  pulseview (0.4.2-21.1)
  python-pylsqpack (0.3.24-1.1)
  python-pyopenssl (26.4.0-1)
  python-service-identity (26.1.0-1)
  python');
INSERT INTO log(t, category, title, detail) VALUES(1791248343.0, 'software', 'Installed cccl, cuda', 'Command: pacman -S --needed cuda
Installed:
  cccl (3.4.2-1)
  cuda (13.4.2-1)
');
INSERT INTO log(t, category, title, detail) VALUES(1791251109.0, 'software', 'Installed gcc15-libs, gcc15; downgraded cuda', 'Command: pacman -U https://archive.archlinux.org/packages/c/cuda/cuda-13.3.1-1-x86_64.pkg.tar.zst
Installed:
  gcc15-libs (15.3.0+r0.g4db0e8df15be-2)
  gcc15 (15.3.0+r0.g4db0e8df15be-2)
Downgraded:
  cuda (13.4.2-1 -> 13.3.1-1)
');
INSERT INTO log(t, category, title, detail) VALUES(1791328592.0, 'software', 'Installed hcxtools, hcxdumptool', 'Command: pacman -S --needed hcxtools hcxdumptool
Installed:
  hcxtools (7.1.2-1.1)
  hcxdumptool (7.1.2-1.1)
');
INSERT INTO log(t, category, title, detail) VALUES(1791639393.0, 'software', 'Removed ninvaders', 'Command: pacman --remove -- ninvaders
Removed:
  ninvaders (0.1.1-3)
');
INSERT INTO log(t, category, title, detail) VALUES(1791729252.0, 'software', 'Removed 5 packages', 'Command: pacman -Rns lib32-libcap
Removed:
  lib32-libcap (2.78-1)
  lib32-pam (1.7.2-1)
  lib32-libnsl (2.0.1-2)
  lib32-libtirpc (1.3.7-1)
  lib32-audit (4.1.4-1)
');
INSERT INTO log(t, category, title, detail) VALUES(1791730661.0, 'software', 'Installed 5 packages', 'Installed:
  libkeyfinder (2.2.8-2.1)
  opusfile (0.12-4.1)
  libid3tag (0.16.4-1.1)
  portmidi (1:2.0.8-1.1)
  mixxx (2.5.6-7)
');
INSERT INTO log(t, category, title, detail) VALUES(1791731097.0, 'software', 'Installed 6 packages', 'Installed:
  libsbsms (2.3.0-6.1)
  portsmf (234-3.1)
  qt5-x11extras (5.15.19-1.1)
  suil (0.10.26-1.1)
  vamp-plugin-sdk (1:2.10-1.1)
  audacity (1:3.7.9-1)
');
INSERT INTO log(t, category, title, detail) VALUES(1791765326.0, 'software', 'Installed 4 packages; upgraded 278 packages', 'Installed:
  cjson (1.7.19-1.1)
  libmicrohttpd (1.0.10-1.1)
  mbedtls3 (3.6.7-1.1)
  librist (0.2.20-1)
Upgraded:
  bcg729 (1.1.1-2.1 -> 1.1.2-1.1)
  glances (4.5.7-1 -> 4.5.8-1)
  perl (5.42.2-2.1 -> 5.42.3-1.1)
  haskell-os-string (2.0.11-71 -> 2.0.11-73)
  haskell-hashable (1.4.7.0-150 -> 1.4.7.0-152)
  haskell-data-fix (0.3.4-272 -> 0.3.4-274)
  haskell-dlist (1.0-485 -> 1.0-486)
  haskell-base-orphans (0.9.4-65 -> 0.9.4-66)
  haskell-generically (0.1.1-150 -> 0.1.1-151)
  haskell-primitive (0.9.1.0-235 -> 0.9.1.0-237)
  haskell-integer-conversion (0.1.1-116 -> 0.1.1-117)
  haskell-integer-logarithms (1.0.4-266 -> 1.0.4-268)
  haskell-th-compat (0.1.7-59 -> 0.1.7-60)
  haskell-network-uri (2.6.4.2-196 -> 2.6.4.2-197)
  haskell-onetuple (0.4.3-44 -> 0.4.3-46)
  haskell-splitmix (0.1.3.2-9 -> 0.1.3.2-10)
  haskell-random (1.2.1.3-352 -> 1.2.1.3-354)
  haskell-quickcheck (2.15.0.1-265 -> 2.15.0.1-267)
  haskell-scientific (0.3.9.0-37 -> 0.3.9.0-39)
  haskell-unordered-containers (0.2.21-59 -> 0.2.21-61)
  haskell-colour (2.3.7-89 -> 2.3.7-91)
  haskell-ansi-terminal-types (1.1-124 -> 1.1-126)
  haskell-ansi-terminal (1.1.2-8 -> 1.1.2-10)
  haskell-prettyprinter (1.7.2-46 -> 1.7.2');
INSERT INTO log(t, category, title, detail) VALUES(1791765479.0, 'software', 'Upgraded accounts-qml-module', 'Command: pacman --upgrade --noconfirm -- /home/user/.cache/paru/clone/accounts-qml-module/accounts-qml-module-0.7-8-x86_64.pkg.tar.zst
Upgraded:
  accounts-qml-module (0.7-6.1 -> 0.7-8)
');
INSERT INTO log(t, category, title, detail) VALUES(1791765548.0, 'software', 'Upgraded litehtml0.9', 'Command: pacman --upgrade --noconfirm -- /home/user/.cache/paru/clone/litehtml0.9/litehtml0.9-0.9-4-x86_64.pkg.tar.zst
Upgraded:
  litehtml0.9 (0.9-2.1 -> 0.9-4)
');
INSERT INTO log(t, category, title, detail) VALUES(1791765656.0, 'software', 'Upgraded msi-ec-dkms-git', 'Upgraded:
  msi-ec-dkms-git (r704.0180060-1 -> r734.d7fbbd8-1)
');
INSERT INTO log(t, category, title, detail) VALUES(1792100600.107911, 'problems', 'ktorrent crashed', 'systemd-coredump: Process 1225410 (ktorrent) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792106070.907519, 'services', 'dbus- failed: 1.2-org.kde.KSplash@1.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@1.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792114122.915975, 'services', 'dbus- failed: 1.2-org.kde.KSplash@2.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@2.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792115000.91626, 'services', 'dbus- failed: 1.2-org.kde.KSplash@3.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@3.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792120097.905901, 'services', 'dbus- failed: 1.2-org.kde.KSplash@4.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@4.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792120136.814164, 'problems', 'ktorrent crashed', 'systemd-coredump: Process 1379894 (ktorrent) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792174933.206522, 'admin', 'Ran as administrator (sudo) by user: /usr/bin/sh -c ''#012install -m 755 /tmp/claude-1000/-home-user/2447b8af-c256-4f9a-bbe0-e894f5410097/scratchpad/cpu-power-limits /usr/local/bin/cpu-power-limits &&#012install -m 644 /tmp/claude-1000/-home-user/2447b8af-c256-4f9a-bbe0-e894f5410097/scratchpad/cpu-power-limits.service /etc/systemd/system/cpu-power-limits.service &&#012install -m 644 /tmp/claude-1000/-home-user/2447b8af-c256-4f9a-bbe0-e894f5410097/scratchpad/99-cpu-power-limits.rules /etc/udev/rules.d/99-cpu-power-limits.rules &&#012systemctl daemon-reload && udevadm control --reload &&#012systemctl enable --now cpu-power-limits.service''', 'sudo:  user : PWD=/home/user ; USER=root ; COMMAND=/usr/bin/sh -c ''#012install -m 755 /tmp/claude-1000/-home-user/2447b8af-c256-4f9a-bbe0-e894f5410097/scratchpad/cpu-power-limits /usr/local/bin/cpu-power-limits &&#012install -m 644 /tmp/claude-1000/-home-user/2447b8af-c256-4f9a-bbe0-e894f5410097/scratchpad/cpu-power-limits.service /etc/systemd/system/cpu-power-limits.service &&#012install -m 644 /tmp/claude-1000/-home-user/2447b8af-c256-4f9a-bbe0-e894f5410097/scratchpad/99-cpu-power-limits.rules /etc/udev/rules.d/99-cpu-power-limits.rules &&#012systemctl daemon-reload && udevadm control --reload &&#012systemctl enable --now cpu-power-limits.service''');
INSERT INTO log(t, category, title, detail) VALUES(1792174934.0483131, 'startup', 'System service added: /etc/systemd/system/cpu-power-limits.service', '+ [Unit]
+ Description=Cap CPU power limits (45 W / 60 W)
+ After=suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target
+ [Service]
+ Type=oneshot
+ ExecStart=/usr/local/bin/cpu-power-limits
+ [Install]
+ WantedBy=multi-user.target suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target
');
INSERT INTO log(t, category, title, detail) VALUES(1792174934.0483131, 'startup', 'Service enabled to start automatically added: /etc/systemd/system/multi-user.target.wants/cpu-power-limits.service', 'Link to /etc/systemd/system/cpu-power-limits.service
+ [Unit]
+ Description=Cap CPU power limits (45 W / 60 W)
+ After=suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target
+ [Service]
+ Type=oneshot
+ ExecStart=/usr/local/bin/cpu-power-limits
+ [Install]
+ WantedBy=multi-user.target suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target
The file can be run as a program.
');
INSERT INTO log(t, category, title, detail) VALUES(1792174934.0483131, 'startup', 'Device rule added: /etc/udev/rules.d/99-cpu-power-limits.rules', '+ SUBSYSTEM=="power_supply", ATTR{type}=="Mains", RUN+="/usr/bin/systemctl --no-block start cpu-power-limits.service"
');
INSERT INTO log(t, category, title, detail) VALUES(1792175132.453371, 'admin', 'Ran as administrator (sudo) by user: /usr/bin/sh -c ''install -m 755 /tmp/claude-1000/-home-user/2447b8af-c256-4f9a-bbe0-e894f5410097/scratchpad/cpu-power-limits /usr/local/bin/cpu-power-limits && sed -i \''s|^Description=.*|Description=Cap CPU power (45 W / 60 W) and clock (3.2 GHz)|\'' /etc/systemd/system/cpu-power-limits.service && systemctl daemon-reload && systemctl start cpu-power-limits.service''', 'sudo:  user : PWD=/home/user ; USER=root ; COMMAND=/usr/bin/sh -c ''install -m 755 /tmp/claude-1000/-home-user/2447b8af-c256-4f9a-bbe0-e894f5410097/scratchpad/cpu-power-limits /usr/local/bin/cpu-power-limits && sed -i \''s|^Description=.*|Description=Cap CPU power (45 W / 60 W) and clock (3.2 GHz)|\'' /etc/systemd/system/cpu-power-limits.service && systemctl daemon-reload && systemctl start cpu-power-limits.service''');
INSERT INTO log(t, category, title, detail) VALUES(1792175133.0482352, 'startup', 'System service added: /etc/systemd/system/cpu-power-limits.service', '+ Description=Cap CPU power (45 W / 60 W) and clock (3.2 GHz)
- Description=Cap CPU power limits (45 W / 60 W)
');
INSERT INTO log(t, category, title, detail) VALUES(1792186508.439602, 'services', 'chromadb.service failed: Failed with result ''protocol''.', 'systemd: chromadb.service: Failed with result ''protocol''.');
INSERT INTO log(t, category, title, detail) VALUES(1792189573.0, 'software', 'Installed xapian-core, python-xapian', 'Command: pacman -S --needed --noconfirm xapian-core python-xapian
Installed:
  xapian-core (1:2.1.0-1.1)
  python-xapian (1:2.1.0-1.1)
');
INSERT INTO log(t, category, title, detail) VALUES(1792273079.0, 'software', 'Installed cpuinfo, onnxruntime-cpu', 'Command: pacman -S --needed --noconfirm onnxruntime-cpu
Installed:
  cpuinfo (r843.ea6b9f1-1.1)
  onnxruntime-cpu (1.29.0-3)
');
INSERT INTO log(t, category, title, detail) VALUES(1792276139.391917, 'problems', 'unllm crashed', 'kernel: unllm[692060]: segfault at 557138911de7 ip 0000557038211e8e sp 00007ffd0222e4e0 error 4 in unllm[1abe8e,557038111000+7e9000] likely on CPU 5 (core 5, socket 0)');
INSERT INTO log(t, category, title, detail) VALUES(1792276213.013547, 'problems', 'unllm crashed 6 times between 18:28:59 and 18:30:13', 'kernel: unllm[692060]: segfault at 557138911de7 ip 0000557038211e8e sp 00007ffd0222e4e0 error 4 in unllm[1abe8e,557038111000+7e9000] likely on CPU 5 (core 5, socket 0)');
INSERT INTO log(t, category, title, detail) VALUES(1792280786.60741, 'problems', 'ktorrent crashed', 'systemd-coredump: Process 695013 (ktorrent) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792283415.400094, 'problems', 'Out of memory: the kernel killed llama-server', 'kernel: Out of memory: Killed process 786966 (llama-server) total-vm:54249008kB, anon-rss:5197292kB, file-rss:77496kB, shmem-rss:103328kB, UID:956 pgtables:15568kB oom_score_adj:0');
INSERT INTO log(t, category, title, detail) VALUES(1792283416.017915, 'services', 'ollama.service failed: Failed with result ''oom-kill''.', 'systemd: ollama.service: Failed with result ''oom-kill''.');
INSERT INTO log(t, category, title, detail) VALUES(1792304750.89947, 'services', 'vte-spawn-9da6894a-23f5-45a0-ab52-fd94435d8454.scope failed: Failed with result ''oom-kill''.', 'systemd: vte-spawn-9da6894a-23f5-45a0-ab52-fd94435d8454.scope: Failed with result ''oom-kill''.');
INSERT INTO log(t, category, title, detail) VALUES(1792447394.243436, 'problems', 'ktorrent crashed', 'systemd-coredump: Process 289146 (ktorrent) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792555811.931184, 'session', 'Lid closed', 'systemd-logind: Lid closed.');
INSERT INTO log(t, category, title, detail) VALUES(1792555813.139775, 'session', 'Went to sleep', 'systemd-sleep: Performing sleep operation ''suspend''...');
INSERT INTO log(t, category, title, detail) VALUES(1792617538.918669, 'session', 'Lid opened', 'systemd-logind: Lid opened.');
INSERT INTO log(t, category, title, detail) VALUES(1792617540.089105, 'session', 'Woke from sleep', 'systemd-sleep: System returned from sleep operation ''suspend''.');
INSERT INTO log(t, category, title, detail) VALUES(1792628229.6585984, 'session', 'Unplugged from power at 98% battery', '');
INSERT INTO log(t, category, title, detail) VALUES(1792628447.744823, 'services', 'vte-spawn-76f4a2c7-8660-4ef6-8b4d-3b911b3fba8f.scope failed: Failed with result ''oom-kill''.', 'systemd: vte-spawn-76f4a2c7-8660-4ef6-8b4d-3b911b3fba8f.scope: Failed with result ''oom-kill''.');
INSERT INTO log(t, category, title, detail) VALUES(1792628727.177998, 'services', 'run-p1169605-i1193312.scope failed: Failed with result ''oom-kill''.', 'systemd: run-p1169605-i1193312.scope: Failed with result ''oom-kill''.');
INSERT INTO log(t, category, title, detail) VALUES(1792628766.939317, 'problems', 'ktorrent crashed', 'systemd-coredump: Process 1102062 (ktorrent) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792629650.0, 'software', 'Installed 6 packages; removed onnxruntime-cpu', 'Installed:
  cudnn (9.27.0.42-1.1)
  cudnn-frontend (1.31.0-1.1)
  nccl (2.32.3-1.1)
  onednn (3.11.3-1)
  onnxruntime-cuda (1.29.0-3)
  sentencepiece (0.2.2-2)
Removed:
  onnxruntime-cpu (1.29.0-3)
');
INSERT INTO log(t, category, title, detail) VALUES(1792630444.728766, 'problems', 'unllm_rerank crashed', 'kernel: unllm_rerank[1195326]: segfault at 14 ip 00007f36775ae4ef sp 00007ffca09b5840 error 4 in libprotobuf-lite.so.36.1.0[ce4ef,7f3677507000+c6000] likely on CPU 9 (core 3, socket 0)');
INSERT INTO log(t, category, title, detail) VALUES(1792630452.504588, 'problems', 'unllm_rerank crashed 2 times between 20:54:04 and 20:54:12', 'kernel: unllm_rerank[1195326]: segfault at 14 ip 00007f36775ae4ef sp 00007ffca09b5840 error 4 in libprotobuf-lite.so.36.1.0[ce4ef,7f3677507000+c6000] likely on CPU 9 (core 3, socket 0)');
INSERT INTO log(t, category, title, detail) VALUES(1792631765.6586308, 'session', 'Plugged into power at 36% battery', '');
INSERT INTO log(t, category, title, detail) VALUES(1792632975.850104, 'session', 'Lid closed', 'systemd-logind: Lid closed.');
INSERT INTO log(t, category, title, detail) VALUES(1792632977.100213, 'session', 'Went to sleep', 'systemd-sleep: Performing sleep operation ''suspend''...');
INSERT INTO log(t, category, title, detail) VALUES(1792665925.937427, 'session', 'Lid opened', 'systemd-logind: Lid opened.');
INSERT INTO log(t, category, title, detail) VALUES(1792665928.954582, 'session', 'Woke from sleep', 'systemd-sleep: System returned from sleep operation ''suspend''.');
INSERT INTO log(t, category, title, detail) VALUES(1792668211.516172, 'services', 'ollama.service failed: Failed with result ''oom-kill''.', 'systemd: ollama.service: Failed with result ''oom-kill''.');
INSERT INTO log(t, category, title, detail) VALUES(1792672669.7134137, 'session', 'Unplugged from power at 98% battery', '');
INSERT INTO log(t, category, title, detail) VALUES(1792674436.7135572, 'session', 'Plugged into power at 75% battery', '');
INSERT INTO log(t, category, title, detail) VALUES(1792678309.330357, 'services', 'vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope failed: Failed with result ''oom-kill''.', 'systemd: vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope: Failed with result ''oom-kill''.');
INSERT INTO log(t, category, title, detail) VALUES(1792680833.998274, 'services', 'ollama.service failed: Failed with result ''oom-kill''.', 'systemd: ollama.service: Failed with result ''oom-kill''.');
INSERT INTO log(t, category, title, detail) VALUES(1792681660.152949, 'session', 'Lid closed', 'systemd-logind: Lid closed.');
INSERT INTO log(t, category, title, detail) VALUES(1792681661.630657, 'session', 'Went to sleep', 'systemd-sleep: Performing sleep operation ''suspend''...');
INSERT INTO log(t, category, title, detail) VALUES(1792686373.942406, 'session', 'Lid opened', 'systemd-logind: Lid opened.');
INSERT INTO log(t, category, title, detail) VALUES(1792686375.111395, 'session', 'Woke from sleep', 'systemd-sleep: System returned from sleep operation ''suspend''.');
INSERT INTO log(t, category, title, detail) VALUES(1792688831.159867, 'session', 'Power button pressed', 'systemd-logind: Power key pressed short.');
INSERT INTO log(t, category, title, detail) VALUES(1792688840.739705, 'services', 'dbus- failed: 1.2-org.kde.kdeconnect@0.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.kdeconnect@0.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792688840.741822, 'services', 'dbus- failed: 1.2-org.kde.kwalletd6@0.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.kwalletd6@0.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792688841.080193, 'session', 'Logged out (Removed session 3.)', 'systemd-logind: Removed session 3.');
INSERT INTO log(t, category, title, detail) VALUES(1792688841.133123, 'session', 'Logged in: ''plasmalogin'' with class ''greeter'' and type ''wayland''.', 'systemd-logind: New session ''c2'' of user ''plasmalogin'' with class ''greeter'' and type ''wayland''.');
INSERT INTO log(t, category, title, detail) VALUES(1792688841.17177, 'session', 'Logged in: ''plasmalogin'' with class ''manager-early'' and type ''unspecified''.', 'systemd-logind: New session ''4'' of user ''plasmalogin'' with class ''manager-early'' and type ''unspecified''.');
INSERT INTO log(t, category, title, detail) VALUES(1792688847.705407, 'session', 'Logged in: ''user'' with class ''user'' and type ''wayland''.', 'systemd-logind: New session ''5'' of user ''user'' with class ''user'' and type ''wayland''.');
INSERT INTO log(t, category, title, detail) VALUES(1792688852.845375, 'session', 'Logged out (Removed session c2.)', 'systemd-logind: Removed session c2.');
INSERT INTO log(t, category, title, detail) VALUES(1792688863.054508, 'session', 'Logged out (Removed session 4.)', 'systemd-logind: Removed session 4.');
INSERT INTO log(t, category, title, detail) VALUES(1792694106.593107, 'problems', 'ascore_tests crashed', 'systemd-coredump: Process 1554824 (ascore_tests) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792694112.695733, 'problems', 'ask crashed', 'systemd-coredump: Process 1554942 (ask) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792694300.592338, 'problems', 'ascore_tests crashed', 'systemd-coredump: Process 1557552 (ascore_tests) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792694307.032216, 'problems', 'ask crashed', 'systemd-coredump: Process 1557670 (ask) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792694312.262506, 'problems', 'ask crashed 2 times between 14:38:27 and 14:38:32', 'systemd-coredump: Process 1557670 (ask) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792694425.114666, 'problems', 'ascore_tests crashed', 'systemd-coredump: Process 1559251 (ascore_tests) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792694430.177295, 'problems', 'ask crashed', 'systemd-coredump: Process 1559275 (ask) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792695283.514082, 'session', 'Lid closed', 'systemd-logind: Lid closed.');
INSERT INTO log(t, category, title, detail) VALUES(1792695284.334464, 'session', 'Lid opened', 'systemd-logind: Lid opened.');
INSERT INTO log(t, category, title, detail) VALUES(1792695285.075141, 'session', 'Went to sleep', 'systemd-sleep: Performing sleep operation ''suspend''...');
INSERT INTO log(t, category, title, detail) VALUES(1792695298.449898, 'session', 'Woke from sleep', 'systemd-sleep: System returned from sleep operation ''suspend''.');
INSERT INTO log(t, category, title, detail) VALUES(1792698580.120238, 'services', 'dbus- failed: 1.2-org.kde.KSplash@2.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@2.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792701191.121459, 'services', 'dbus- failed: 1.2-org.kde.KSplash@3.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@3.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792701592.120236, 'services', 'dbus- failed: 1.2-org.kde.KSplash@4.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@4.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792705595.120515, 'services', 'dbus- failed: 1.2-org.kde.KSplash@5.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@5.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792705878.109515, 'services', 'dbus- failed: 1.2-org.kde.KSplash@6.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@6.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792705948.121416, 'services', 'dbus- failed: 1.2-org.kde.KSplash@7.service: Failed with result ''exit-code''.', 'systemd: dbus-:1.2-org.kde.KSplash@7.service: Failed with result ''exit-code''.');
INSERT INTO log(t, category, title, detail) VALUES(1792709057.0572298, 'startup', 'Program in ~/.local/bin added: /home/user/.local/bin/claude', 'Link to /home/user/.local/share/claude/versions/2.1.309
The file can be run as a program.
');
INSERT INTO log(t, category, title, detail) VALUES(1792709057.0572298, 'startup', 'Program in ~/.local/bin removed: /home/user/.local/bin/claude.tmp.1497955.1792709056263', '');
INSERT INTO log(t, category, title, detail) VALUES(1792709130.0560093, 'startup', 'Program in ~/.local/bin added: /home/user/.local/bin/claude', 'Link to /home/user/.local/share/claude/versions/2.1.309
The file can be run as a program.
');
INSERT INTO log(t, category, title, detail) VALUES(1792709130.0560093, 'startup', 'Program in ~/.local/bin removed: /home/user/.local/bin/claude.tmp.1497737.1792709129957', '');
INSERT INTO log(t, category, title, detail) VALUES(1792724190.374719, 'problems', 'ktorrent crashed', 'systemd-coredump: Process 1843640 (ktorrent) of user 1000 dumped core.');
INSERT INTO log(t, category, title, detail) VALUES(1792726199.97967, 'session', 'Unplugged from power at 100% battery', '');
INSERT INTO log(t, category, title, detail) VALUES(1792727181.9801161, 'session', 'Plugged into power at 88% battery', '');
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1, 1791327267.1, 1791929731.8577728, 'systemd', '', '/usr/lib/systemd/systemd --switched-root --system --deserialize=57 splash', 'root', 'init.scope', 0, '', 17.2, 12206080.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(885, 1791327278.24, 1791929731.8577728, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 1, 'systemd', 261.15, 2011136.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1, 1791730545.11, 1792186416.928317, 'systemd', '', '/usr/lib/systemd/systemd --switched-root --system --deserialize=57 splash', 'root', 'init.scope', 0, '', 17.79, 15634432.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(877, 1791730554.77, 1792186416.928317, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 1, 'systemd', 499.76, 4632576.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(508603, 1791852574.58, 1791852576.4278374, 'pacman', '/usr/bin/pacman', 'pacman -Qo /usr/local/sbin/fan-control.sh', 'user', 'vte-spawn-35664242-a3f2-4a4d-a97e-b56e4a1ae71c.scope', 508480, 'python3', 0.01, 14307328.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(784611, 1792021503.42, 1792021507.6077187, 'pacman', '/usr/bin/pacman', 'pacman -Qo /usr/local/sbin/fan-control.sh', 'user', 'vte-spawn-35664242-a3f2-4a4d-a97e-b56e4a1ae71c.scope', 784504, 'python3', 0.03, 18436096.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(785260, 1792021536.45, 1792021538.6081467, 'pacman', '/usr/bin/pacman', 'pacman -Qo /usr/local/sbin/fan-control.sh', 'user', 'vte-spawn-35664242-a3f2-4a4d-a97e-b56e4a1ae71c.scope', 785149, 'python3', 0.02, 14360576.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1999596, 1792175150.94, 1792175152.0480554, '(cpu-power-lim)', '', '(u-power-limits)', 'root', 'cpu-power-limits.service', 1, 'systemd', 0.0, 7630848.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1, 1792186489.11, 1792375592.5090694, 'systemd', '', '/usr/lib/systemd/systemd --switched-root --system --deserialize=57 splash', 'root', 'init.scope', 0, '', 1.17, 15572992.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(878, 1792186501.08, 1792375592.5090694, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 1, 'systemd', 0.01, 4644864.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(233827, 1792200661.75, 1792200665.0850115, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 878, 'fan-control.sh', 0.0, 143360.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(271020, 1792235353.49, 1792235355.8620062, '(cpu-power-lim)', '', '(u-power-limits)', 'root', 'cpu-power-limits.service', 1, 'systemd', 0.03, 8122368.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(711754, 1792277083.53, 1792277085.8619957, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 878, 'fan-control.sh', 0.0, 1798144.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(711755, 1792277083.53, 1792277085.8619957, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 711754, 'fan-control.sh', 0.0, 1708032.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1, 1792408864.11, 1792464285.7020624, 'systemd', '', '/usr/lib/systemd/systemd --switched-root --system --deserialize=57 splash', 'root', 'init.scope', 0, '', 1.04, 15343616.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(906, 1792408875.93, 1792464285.7020624, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 1, 'systemd', 0.08, 4595712.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(2065, 1792408908.18, 1792464285.7020624, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 796, 'systemd', 2.44, 390176768.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(289146, 1792431582.61, 1792447394.7020497, 'ktorrent', '/usr/bin/ktorrent', '/usr/bin/ktorrent magnet:?xt=urn:btih:0000', 'user', 'app-firefox@083a2342d0e2483e84ecc8eb15d679cc.service', 796, 'systemd', 90.29, 244101120.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1, 1792464425.11, 1792700260.2240815, 'systemd', '', '/usr/lib/systemd/systemd --switched-root --system --deserialize=57 splash', 'root', 'init.scope', 0, '', 11.37, 15216640.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1, 1792464427.11, NULL, 'systemd', '', '/usr/lib/systemd/systemd --switched-root --system --deserialize=57 splash', 'root', 'init.scope', 0, '', 57.71, 11116544.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(926, 1792464435.0, 1792700260.2240815, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 1, 'systemd', 363.87, 4669440.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(926, 1792464437.0, NULL, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 1, 'systemd', 432.9, 3010560.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1692, 1792464442.15, 1792688836.0884387, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 4751.67, 375209984.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1102062, 1792624329.2, 1792628769.6593287, 'ktorrent', '/usr/bin/ktorrent', '/usr/bin/ktorrent magnet:?xt=urn:btih:0000', 'user', 'app-firefox@6bf20d5ba3094fa88cbd1c32bbb7740e.service', 774, 'systemd', 90.94, 253702144.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1102691, 1792624359.42, 1792624362.658597, 'ktorrent', '/usr/bin/ktorrent', '/usr/bin/ktorrent magnet:?xt=urn:btih:0000', 'user', 'app-firefox@6bf20d5ba3094fa88cbd1c32bbb7740e.service', 774, 'systemd', 0.31, 92598272.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1166967, 1792628445.81, 1792628447.6575835, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 926, 'fan-control.sh', 0.0, 274432.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1276284, 1792668213.82, 1792680835.71352, 'ollama', '', '/usr/local/bin/ollama serve', 'ollama', 'ollama.service', 1, 'systemd', 698.78, 69136384.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1307324, 1792670456.23, 1792670459.7119277, 'ollama', '/usr/local/bin/ollama', 'ollama show --modelfile odysseus-stable2:8b', 'user', 'vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope', 1307321, 'zsh', 0.01, 37003264.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1310981, 1792670707.7, 1792670711.7129555, 'ollama', '/usr/local/bin/ollama', 'ollama show --template qwen3:4b', 'user', 'vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope', 1310980, 'zsh', 0.06, 32354304.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1455323, 1792680836.33, 1792700260.2240815, 'ollama', '', '/usr/local/bin/ollama serve', 'ollama', 'ollama.service', 1, 'systemd', 52.99, 44404736.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1455323, 1792680838.33, NULL, 'ollama', '', '/usr/local/bin/ollama serve', 'ollama', 'ollama.service', 1, 'systemd', 110.3, 41730048.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1497162, 1792688849.67, 1792698520.224022, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 323.01, 388505600.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1610015, 1792698518.82, 1792698558.2230184, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 5.24, 441573376.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1610728, 1792698556.52, 1792701130.072035, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 86.37, 385024000.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1640808, 1792701129.29, 1792701534.072026, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 22.51, 440254464.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1646595, 1792701531.09, 1792701558.072877, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 7.87, 429268992.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1646989, 1792701556.86, 1792705534.0790143, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 122.23, 371703808.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1689256, 1792705533.96, 1792705817.4704416, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 15.02, 454828032.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1693063, 1792705816.46, 1792705862.4690025, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 8.23, 427720704.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1693666, 1792705861.51, 1792705889.4699292, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 9.34, 425824256.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1694052, 1792705886.5, 1792705907.4699767, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 7.76, 422948864.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1694398, 1792705906.39, NULL, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 396.94, 450064384.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1843640, 1792719238.67, 1792724191.980281, 'ktorrent', '/usr/bin/ktorrent', '/usr/bin/ktorrent magnet:?xt=urn:btih:0000', 'user', 'app-firefox@fc022eb11b464f22a3cd3a4e2aea0542.service', 774, 'systemd', 58.06, 165228544.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1918646, 1792726449.75, 1792726452.9803169, 'pacman', '/usr/bin/pacman', 'pacman -Qo /usr/local/sbin/fan-control.sh', 'user', 'vte-spawn-6c737254-0340-4ab7-96a7-cee48837ac5f.scope', 1918645, 'zsh', 0.03, 14655488.0);
INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(1931042, 1792727589.74, 1792727592.9809978, 'pacman', '/usr/bin/pacman', 'pacman -Qo /usr/local/sbin/fan-control.sh', 'user', 'vte-spawn-6c737254-0340-4ab7-96a7-cee48837ac5f.scope', 1931041, 'bash', 0.07, 14491648.0);
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(19, 1790297725.735651, 1790297725.735651, 32, 32, '20:55:25 to 20:55:25 (1 s). The machine stopped without warning on Thu Sep 24 at 20:55:25. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. ActivityScope was not recording then, so there are no readings from just before.');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(20, 1790299126.423578, 1790299126.423578, 32, 32, '21:18:46 to 21:18:46 (1 s). The machine stopped without warning on Thu Sep 24 at 21:18:46. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. ActivityScope was not recording then, so there are no readings from just before.');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(21, 1790299879.364283, 1790299879.364283, 32, 32, '21:31:19 to 21:31:19 (1 s). The machine stopped without warning on Thu Sep 24 at 21:31:19. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. ActivityScope was not recording then, so there are no readings from just before.');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(22, 1790459361.46332, 1790459361.46332, 32, 32, '17:49:21 to 17:49:21 (1 s). The machine stopped without warning on Sat Sep 26 at 17:49:21. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. ActivityScope was not recording then, so there are no readings from just before.');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(23, 1790460207.327931, 1790460207.327931, 32, 32, '18:03:27 to 18:03:27 (1 s). The machine stopped without warning on Sat Sep 26 at 18:03:27. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. ActivityScope was not recording then, so there are no readings from just before.');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(24, 1790649514.685113, 1790649514.685113, 32, 32, '22:38:34 to 22:38:34 (1 s). The machine went to sleep on Mon Sep 28 at 22:38:34 and never woke up; the battery may have run out. ActivityScope was not recording then, so there are no readings from just before.');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(25, 1791255429.322559, 1791255429.322559, 32, 32, '22:57:09 to 22:57:09 (1 s). The machine stopped without warning on Mon Oct  5 at 22:57:09. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. ActivityScope was not recording then, so there are no readings from just before.');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(26, 1791257237.466935, 1791257237.466935, 32, 32, '23:27:17 to 23:27:17 (1 s). The machine stopped without warning on Mon Oct  5 at 23:27:17. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. ActivityScope was not recording then, so there are no readings from just before.');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(27, 1791295721.531929, 1791295721.531929, 48, 32, '10:08:41 to 10:08:41 (1 s). The system shut itself down for heat on Tue Oct  6 at 10:08:41: the kernel''s shutdown sensor reached its critical temperature. ActivityScope was not recording then, so there are no readings from just before.');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(595, 1792186418.252501, 1792186418.252501, 32, 32, '17:33:38 to 17:33:38 (1 s). The machine stopped without warning on Fri Oct 16 at 17:33:38. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. The last reading, 1 s before, was 89 °C on the shutdown sensor (11 °C below its 100 °C limit), with the CPU 16% busy. The busiest program in its last two minutes was llama-server (34% of the CPU time).');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(1084, 1792375539.390384, 1792375539.390384, 32, 32, '22:05:39 to 22:05:39 (1 s). The machine stopped without warning on Sun Oct 18 at 22:05:39. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. The last reading, -1 s before, was 74 °C on the shutdown sensor (26 °C below its 100 °C limit), with the CPU 1% busy. The busiest program in its last two minutes was python (93% of the CPU time).');
INSERT INTO events(id, start, "end", kinds, primary_kind, summary) VALUES(1153, 1792464270.223397, 1792464270.223397, 32, 32, '22:44:30 to 22:44:30 (1 s). The machine stopped without warning on Mon Oct 19 at 22:44:30. Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, the battery dies, or the system freezes. The last reading, -1 s before, was 96 °C on the shutdown sensor (4 °C below its 100 °C limit), with the CPU 37% busy. The busiest program in its last two minutes was python (25% of the CPU time).');
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668200.71292, 85.40263922201473, 8283066368.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668201.7128665, 86.21105023709384, 8283062272.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668202.712904, 87.16249142993338, 8283062272.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668203.712937, 87.87981805869109, 8283062272.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668204.7119644, 88.53765797911834, 8283062272.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668205.7119215, 88.11912472948507, 8283062272.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668206.7132528, 88.07266773657031, 8283062272.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668207.7128792, 88.18689408423724, 8283058176.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668208.7131815, 88.25931161071266, 8283058176.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668209.7128808, 88.7030389400586, 8283058176.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668210.7131486, 89.95082580457256, 8283058176.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668211.7129362, 83.12046107885548, 8192884736.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668212.7137148, 37.4441632190666, 6227353600.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668213.7129135, 37.94443506293946, 6227353600.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792668214.7119207, 38.32410168518477, 6227353600.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680823.712917, 89.1146246564247, 8283222016.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680824.7136612, 89.21577184004059, 8283209728.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680825.7129335, 89.1555928508152, 8283209728.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680826.712851, 89.18296267048345, 8283156480.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680827.713747, 89.2097885551628, 8282988544.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680828.711861, 89.77800282401157, 8283054080.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680829.7128358, 89.75822336987011, 8282873856.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680830.7129216, 89.74999017208373, 8283209728.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680831.7129297, 89.93465610081192, 8283209728.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680832.712927, 89.7875711349525, 8283099136.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680833.7128336, 89.87576277610573, 8283009024.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680834.7132225, 38.81324758610305, 6707023872.0);
INSERT INTO samples(t, mem_pct, swap_used) VALUES(1792680835.71352, 38.78187242697116, 6707019776.0);
