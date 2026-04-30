# smoothfs operator runbook

Dagelijkse procedures voor het draaien van smoothfs in de SmoothNAS
appliance-integratie. Behandelt installatie, pool lifecycle, share creation,
routine onderhoud, kernel upgrades en troubleshooting. Gebruik samen met
`smoothfs-support-matrix.md` voor de geteste versie-combinaties.

[English version](smoothfs-operator-runbook.md)

## 1. Eerste installatie

Op een verse Debian 13 appliance:

```bash
# 1. Installeer de drie debs. Volgorde is belangrijk — tierd Recommends (niet
#    Depends) de andere twee, dus apt installeert ze niet automatisch.
#    smoothfs-dkms is Architecture: all; de andere twee worden gepubliceerd
#    voor amd64 en arm64 — kies de deb die bij de appliance-architectuur past.
arch="$(dpkg --print-architecture)"
apt install ./smoothfs-dkms_0.1.0-1_all.deb
apt install ./smoothfs-samba-vfs_0.1.0-1_${arch}.deb
apt install ./tierd_0.1.0-1_${arch}.deb

# 2. Controleer of de kernel module gebouwd en geladen is.
bash /usr/share/smoothfs-dkms/kernel_upgrade.sh
bash /usr/share/smoothfs-dkms/module_signing.sh

# 3. Op secure-boot hosts, installeer de DKMS MOK cert. Dit vereist
#    een reboot + shim prompt — plan dit op tijd.
bash /usr/share/smoothfs-dkms/enroll-signing-cert.sh
# (herstart; bevestig in de shim MOK-management UI met het wachtwoord dat
#  ingevoerd is tijdens --import)

# 4. Bevestig dat tierd draait.
systemctl status tierd
curl -s http://127.0.0.1:8420/api/health
```

Op dit punt heeft de appliance nog geen smoothfs pools. Log in op de web UI
(default 8420) of gebruik `tierd-cli` om er één aan te maken.

## 2. Een smoothfs pool aanmaken

Smoothfs pools worden door operators declared — tierd auto-start niet automatisch uit disk.
Elke pool heeft een naam, een UUID (indien niet opgegeven auto gegenereerd),
en een geordende lijst van tier mountpoints (snelste eerst).

### Via de web UI

Storage → smoothfs Pools → **Create Pool**. Vul in:

- **Name** — lowercase alnum + `._-`, ≤ 63 tekens.
- **UUID** — laat leeg voor auto-generatie.
- **Tier paths** — regels of dubbele punt gescheiden. Moeten bestaande directories zijn. Snelste eerst.

tierd schrijft een systemd mount unit op `/etc/systemd/system/mnt-smoothfs-<name>.mount`,
enable + start deze. Phase 2.5 auto-discovery verbindt de planner.

### Via CLI

```bash
tierd-cli smoothfs create-pool \
    --name tank \
    --tiers /mnt/nvme-fast:/mnt/sas-slow
```

CLI gebruikt de library direct, niet de REST. CLI-gecreateerde pools verschijnen in de REST list
nadat mount-event auto-discovery van tierd gestart is (meestal < 1 s na succesvolle mount).

### Wat je zou moeten zien

```bash
mount | grep smoothfs                        # smoothfs pool mounted
ls -la /etc/systemd/system/mnt-smoothfs-*.mount  # unit file written
systemctl status mnt-smoothfs-tank.mount     # active (mounted)
curl -s http://127.0.0.1:8420/api/smoothfs/pools  # persisted row
```

## 3. Shares aanmaken op een pool

Zodra de pool gemount is op `/mnt/smoothfs/<name>`, is het een normale POSIX path.
Maak shares met de bestaande Sharing flows:

- **NFS / SMB** — Sharing → Add Share, set pad naar `/mnt/smoothfs/<name>/<subdir>`.
- **iSCSI (file-backed LUN)** — Sharing → iSCSI → Add Target → **File-backed**. Zet Backing File op een absoluut pad naar een bestaand file onder `/mnt/smoothfs/<name>/`. De file wordt direct gepind met `PIN_LUN` wanneer tierd LIO aanroept.

Via CLI:

```bash
# Maak eerst een sizing-backed file.
truncate -s 256G /mnt/smoothfs/tank/luns/web-app.img

# Maak daarna de LIO target.
tierd-cli iscsi create-fileio \
    --iqn iqn.2026-04.com.smoothnas:web-app \
    --file /mnt/smoothfs/tank/luns/web-app.img
```

`getfattr -n trusted.smoothfs.lun /mnt/smoothfs/tank/luns/web-app.img` moet `0x01` teruggeven.

## 4. Routine onderhoud

### Quiesce (bewegingsactiviteiten pauzeren)

Stopt in-flight cutovers + weigert nieuwe `MOVE_PLAN`s. Gebruik dit voor ingrepen op
een actieve pool (handmatige `cp` tussen tiers, handmatige `setfattr`, enz.). Phase 2
maakt quiesce veilig op een live pool — readers + writers werken door, alleen movement stopt.

```bash
# UI: per-pool Quiesce knop.
# CLI:
tierd-cli smoothfs quiesce --pool <uuid>
```

### Reconcile (movement hervatten)

Heft quiesce op + heractiveert heat drain.

```bash
# UI: per-pool Reconcile knop (met redenprompt — vastgelegd in movement log).
# CLI:
tierd-cli smoothfs reconcile --pool <uuid> --reason "handmatige controle klaar"
```

### Active-LUN movement (Phase 8)

De normale movement overslaat `PIN_LUN` objecten. Active-LUN movement is een
opt-in, target-gequiesce flow:

1. Quiesce/stop de iSCSI target zodat actieve I/O afloopt.
2. Zorg dat `trusted.smoothfs.lun` door SmoothNAS uit de backing file is gehaald
   vóórdat er een movement plan wordt gemaakt.
3. Vraag via de SmoothNAS controlelaag een prepared movement aan voor dat backing
   object (de CLI/UI voor deze aanvraag zit buiten deze repo).
4. tierd kopieert en switched, plaatst daarna opnieuw `trusted.smoothfs.lun` op de
   destination en hervat de target pas als de destination pin controle slaagt.

Controleer in de standaard levenscyclus tooling:

- `smoothfs_movement_log` bevat `failed` wanneer destination verificatie of resume
  faalt.
- `pin_state='pin_lun'` in `smoothfs_objects` toont pin-status na recovery:
    ```bash
    sqlite3 /var/lib/tierd/tierd.db \
      "SELECT object_id, pin_state, movement_state FROM smoothfs_objects WHERE pin_state='pin_lun' ORDER BY updated_at DESC LIMIT 20;"
    ```
- `getfattr -n trusted.smoothfs.lun <destination-pad>` moet `0x01` tonen.

Als tierd tijdens een move opnieuw start, reconcilieert recovery de movement-states
en past PINs indien mogelijk opnieuw toe vóór het hervatten van normale movement.

### Movement log

Storage → smoothfs Pools → Movement log (onder pool lijst). Toont de laatste 100 state
transities uit `smoothfs_movement_log` van alle pools. Elke rij toont state transition,
object_id en source/dest tier. Gebruik dit om te controleren dat quiesce planner-activiteit stopt
en reconcile dat herstart.

Directe SQLite query (handig voor scripting):

```bash
sqlite3 /var/lib/tierd/tierd.db \
    'SELECT written_at, to_state, source_tier, dest_tier FROM smoothfs_movement_log ORDER BY id DESC LIMIT 50;'
```

### Write staging

SmoothNAS regelt write staging vanuit de smoothfs Pools pagina. De kernel switches en tellers
ziet u ook onder `/sys/fs/smoothfs/<pool-uuid>/`:

```bash
cat /sys/fs/smoothfs/<uuid>/write_staging_supported
cat /sys/fs/smoothfs/<uuid>/write_staging_enabled
cat /sys/fs/smoothfs/<uuid>/write_staging_full_pct
cat /sys/fs/smoothfs/<uuid>/staged_bytes
cat /sys/fs/smoothfs/<uuid>/staged_rehome_bytes
cat /sys/fs/smoothfs/<uuid>/range_staged_bytes
cat /sys/fs/smoothfs/<uuid>/range_staged_writes
cat /sys/fs/smoothfs/<uuid>/staged_rehomes_total
cat /sys/fs/smoothfs/<uuid>/staged_rehomes_pending
cat /sys/fs/smoothfs/<uuid>/write_staging_drainable_rehomes
cat /sys/fs/smoothfs/<uuid>/write_staging_drain_pressure
cat /sys/fs/smoothfs/<uuid>/write_staging_drainable_tier_mask
cat /sys/fs/smoothfs/<uuid>/metadata_active_tier_mask
cat /sys/fs/smoothfs/<uuid>/write_staging_drain_active_tier_mask
cat /sys/fs/smoothfs/<uuid>/metadata_tier_skips
cat /sys/fs/smoothfs/<uuid>/range_staging_recovery_supported
cat /sys/fs/smoothfs/<uuid>/range_staging_recovered_bytes
cat /sys/fs/smoothfs/<uuid>/range_staging_recovered_writes
cat /sys/fs/smoothfs/<uuid>/range_staging_recovery_pending
cat /sys/fs/smoothfs/<uuid>/recovered_range_tier_mask
cat /sys/fs/smoothfs/<uuid>/oldest_recovered_write_at
cat /sys/fs/smoothfs/<uuid>/last_recovery_at
cat /sys/fs/smoothfs/<uuid>/last_recovery_reason
```

Het eerste data-plane pad behandelt replace-style writes: met staging ingeschakeld,
een `O_TRUNC` write op een regulier bestand op een koudere tier wordt eerst hergeplaatst
naar de snelste tier voordat de cold lower file opent, mits de snelste tier onder
`write_staging_full_pct` (default 98) blijft. Nieuwe bestanden volgen reeds dezelfde regel:
ze landen op de snelste tier totdat deze drempel bereikt is, daarna op de volgende tier.
Range-level staging voor niet-truncating buffered writes wordt ondersteund voor ongepinde
cold-tier reguliere bestanden.
`staged_rehomes_total` telt deze truncate-write rehomes.
`staged_rehome_bytes` rapporteert bytes geschreven via truncate-rehome staging.
Voor ongepinde cold-tier files kunnen buffered non-truncating writes changed ranges in de
snelste tier zetten en via range-merge teruglezen; `range_staged_bytes` en
`range_staged_writes` rapporteren dat gedrag. Zodra een bestand staged ranges heeft,
worden direct I/O en mmap geweigerd zodat callers de merge layer niet omzeilen en
stale bytes lezen. Range-staged metadata wordt onder de `.smoothfs` sidecar area
op de snelste tier bewaard en op remount gereplayed; herstelde ranges blijven
pending totdat SmoothNAS de source tier drain-active markeert.
`staged_rehomes_pending` telt truncate-write rehomes met nog uit te voeren cleanup.
`write_staging_drainable_rehomes` telt staged truncate rehomes waarvan de originele
tier momenteel toegestaan is via `write_staging_drain_active_tier_mask`.
`write_staging_drain_pressure` staat op `1` enkel als staged werk bestaat en de
snelste tier de ingestelde full threshold bereikt heeft.
`write_staging_drainable_tier_mask` toont non-fast tiers met staged werk en SmoothNAS
drain permissie. Dit is enkel een status-signal; lezen hiervan start geen drain.

SmoothNAS kan ook `metadata_active_tier_mask` schrijven om metadata-only walks van standby tiers te onderdrukken.
Bit `0` is altijd geforceerd aan op de snelste tier.
Voor een pool met 2 tiers, het schrijven van `0x1` houdt browse/readdir fallback op de
snelle tier en slaat tier 1 over tot SmoothNAS externe activiteit observeert.
Als een cold-tier dentry al resolved is, geeft `stat` de gecachte smoothfs inode attributen
terug in plaats van ophalen vanuit inactieve lower tier.

De afzonderlijke `write_staging_drain_active_tier_mask` is de data-drain gate.
SmoothNAS moet een bit alleen schrijven nadat het activiteit op het tier's backing device
has geobserveerd. De snelste-tier bit is altijd forced on door kernel.
Als een koudere tier drain-active wordt, voert smoothfs ook truncate-rehome staging cleanup uit
voor die tier door originele lower file te verwijderen en staged state te wissen.
Range-staged writes gebruiken dezelfde gate: smoothfs kopieert staged ranges terug naar de
source lower file, fsynct, verwijdert de snelste-tier sidecar, en wist staged-range state.
Range drain gebruikt deze mask in plaats van de metadata browse mask, zodat directory zichtbaar-heid
en data drain rechten onafhankelijk kunnen verschuiven.

### Pool vernietigen

Stopt en verwijdert de systemd mount unit. Elk share dat naar een bestand op de pool wijst,
geeft `EIO` totdat de pool opnieuw aangemaakt wordt.

```bash
# UI: per-pool Destroy knop.
# CLI:
tierd-cli smoothfs destroy-pool --name tank
```

De tier lower directories blijven ongemoeid — `destroy-pool` verwijdert alleen het smoothfs overlay.
Hercreeren met dezelfde naam + UUID + tiers resurrecteert pool met alle data.

## 5. Kernel upgrades

`apt upgrade` trekt een nieuw `linux-headers-*` package. DKMS autoinstall hooks bouwen smoothfs
voor de nieuwe kernel — geen handmatige stap nodig — zolang kernel `≥ 6.18` is (zie
`BUILD_EXCLUSIVE_KERNEL` in support matrix).

Na upgrade klaar is, voer kernel-upgrade harness uit:

```bash
bash /usr/share/smoothfs-dkms/kernel_upgrade.sh
```

De harness controleert dat elke geïnstalleerde kernel een signed smoothfs module heeft onder
`/lib/modules/<kver>/updates/dkms/smoothfs.ko.xz` of een schone "out of BUILD_EXCLUSIVE_KERNEL" skip;
geen halfbouw of failed state. Als een kernel gebouwd heeft maar niet ondertekend is, dan
vraagt `module_signing.sh` dat meteen.

### Rollback

Per-kernel DKMS trees zorgen dat een failed build op kernel B kernel A's werkende `.ko` niet beïnvloedt.
Als de nieuw geïnstalleerde kernel niet start of smoothfs niet laadt, kies de vorige kernel in GRUB — de module staat daar nog.
Boot dan, en `apt remove` het foutieve linux-headers package om herhaalde rebuilds te voorkomen.

## 6. Samba upgrades

Omdat `smoothfs-samba-vfs` `Depends: samba (= <exact version>)` pin, weigert apt een Samba-upgrade
zonder bijpassend VFS deb. Rebuild de VFS module tegen de nieuwe Samba versie:

```bash
# Op build host (CI, niet de appliance):
apt-get source samba=<new-version>
cd /path/to/smoothfs/samba-vfs
dpkg-buildpackage -us -uc -b

# Op de appliance (gebruik de deb die bij de appliance-architectuur past — amd64 of arm64):
apt install ./smoothfs-samba-vfs_0.1.0-1_$(dpkg --print-architecture).deb
systemctl reload smbd    # laadt de nieuwe .so bij nieuwe verbindingen
```

## 7. Troubleshooting

### "smoothfs: active mounts present; leaving running module in place"

`smoothfs-dkms` prerm schreef dit tijdens `apt upgrade`. Dit is verwacht — pakket liet
de draaiende module staan omdat er nog mounts actief zijn. Het nieuwe `.ko.xz` staat al op disk
voor de volgende reboot. Om direct te activeren: alle smoothfs pools verwijderen + `modprobe -r smoothfs && modprobe smoothfs`.

### `mount -t smoothfs ...` geeft `-EOPNOTSUPP`

Lower filesystem gaat niet door capability gate. Check `dmesg`:

```
smoothfs: tier /mnt/foo has s_magic 0xXXXX; only xfs, ext4, btrfs, zfs are supported
```

Mount de tier op een ondersteund filesystem en probeer opnieuw.

### Samba VFS module faalt met "version SAMBA_X.Y.Z_PRIVATE_SAMBA not found"

Samba is opgewaardeerd zonder VFS deb rebuild. apt had dit moeten blokkeren (zie §6). Als dit toch gebeurt,
pin je Samba naar versie die overeenkomt met geïnstalleerde `smoothfs-samba-vfs`, of rebuild de VFS.

### `/var/lib/dkms/mok.*` ontbreekt op secure boot systemen

Komt voor op verse installs als DKMS framework.conf geen autogeneratie deed.
Genereer handmatig:

```bash
dkms generate_mok
bash /usr/share/smoothfs-dkms/enroll-signing-cert.sh
# reboot + shim prompt
```

### Movement wedged — quiesce komt niet terug

Check `smoothfs_movement_log` op laatste regels. Als een regel blijft staan op
`cutover_in_progress` zonder `switched`/`failed` opvolger, dan wacht kernel op een SRCU
writer. `ss -tnlp | grep :<target-port>` toont het proces dat vasthoudt; stop dat proces indien passend.
Als laatste redmiddel: `modprobe -r smoothfs` nadat elke pool is ontkoppeld forceert recovery op next mount.

### `Inspect` geeft `ENOENT` / UI toont "pool not found"

Mount unit is niet actief. `systemctl start mnt-smoothfs-<name>.mount` en controleer
`journalctl -u mnt-smoothfs-<name>.mount` op mount fout.

### Disaster recovery: tierd.db corruption

De SmoothNAS sqlite db op `/var/lib/tierd/tierd.db` is een MIRROR van state die primair
elders leeft (systemd units on disk, kernel mounts, LIO saveconfig.json). Verlies van deze db
betekent verlies van REST view maar **geen** verlies van pools.

Herstel:

```bash
systemctl stop tierd
mv /var/lib/tierd/tierd.db{,.broken}
systemctl start tierd
# tierd voert goose migrations uit op een lege db.
# Phase 2.5 auto-discovery repopuleert smoothfs_objects vanaf de
# huidig gemounte pools. iSCSI rows moeten handmatig opnieuw worden ingeladen
# (of opnieuw gemaakt via REST API) want LIO saveconfig.json is bron van truth.
```

Geen data op tier lowers staat onder dreiging tijdens een tierd.db recovery — alleen de tierd view.

## 8. Wanneer support te bellen

Escaleer bij:

- Data-integriteit gerelateerde errors in `dmesg`: `smoothfs: cutover lost lower_path`, `smoothfs: placement log corrupted`, `smoothfs: oid mismatch on ...`
- Een pool die niet meer mount na kernel die eerder werkte.
- Een smoothfs module die laadt maar `modprobe smoothfs` direct panics op eerste mount.
- `smbtorture` MUST_PASS regressie (vroeger 16/16).
- Elke movement overgang naar `failed` die niet verdwijnt bij reconcile.

Verzamel dit voor escalatie:

```bash
tar czf /tmp/smoothfs-diag.tar.gz \
    /var/log/syslog* \
    /var/lib/tierd/tierd.db \
    /var/lib/dkms/smoothfs \
    /etc/systemd/system/mnt-smoothfs-*.mount \
    /etc/dkms/framework.conf.d/ \
    <(dmesg --time-format=iso) \
    <(uname -a) \
    <(modinfo smoothfs) \
    <(dkms status)
```
