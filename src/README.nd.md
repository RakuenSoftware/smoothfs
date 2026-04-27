# smoothfs runtime en kernel-integratie deep dive

Deze map bevat de runtime-implementatie die SmoothNAS-beleid in de kernel zichtbaar
maakt.

## Architectuuroverzicht

`controlplane` is de policy-engine. `src/smoothfs` is de filesystem-driver en
systeemintegratie-laag.

```text
Client I/O (NFS / SMB / iSCSI fileio)
        │
        ▼
smoothfs kernel module (gestapelde namespace)
        │
  per-bestands xattrs, plaatsing, beweging
  en lower-tier padresolutie
        │
Onderliggende filesystems (XFS / ext4 / btrfs / zfs)
```

### Verantwoordelijkheden per laag

- **Kernellaag (`src/smoothfs`)**
  - Presenteert een synthetisch filesystem (`smoothfs`) met gestapelde lower files.
  - Resolveert namespace-objecten uit permanente metadata:
    `trusted.smoothfs.oid` (UUIDv7), generatiehulpvelden, gecachte inode-metadata.
  - Handhaaft beweeggedrag via movement-state transities in VFS-operaties.
  - Exposeert control-plane attributen via een netlink family (`uapi_smoothfs.h`).
  - Implementeert data-pad specialisaties zoals heat sampling en write staging.

- **Userspace laag (`controlplane`)**
  - Ontdekt gemounte pools en reconcilieert plannerstaat.
  - Berekent movement candidates op basis van heat en beleid.
  - Voert cutover uit in een journaled flow:
    `plan_accepted -> copy_in_progress -> copy_verified -> cutover_in_progress -> switched`.
  - Herstelt opstarten voor in-flight niet-terminale states.
  - Persisteert en exposeert movement/audit records in SQLite.

- **Samba VFS integratie (`src/smoothfs/samba-vfs`)**
  - Verzorgt lease pin/notification semantiek voor SMB-correctheid tijdens
    movement-scenario's.

- **LIO/iSCSI integratie (`controlplane` + `tierd` consumers)**
  - Gebruikt `trusted.smoothfs.lun` (`PIN_LUN`) voor veiligheid van LUN backing files.
  - Past actieve LUN-verplaatsing alleen toe via operator-gecontroleerde flow.

## Hoofd datastroom

1. Een bestand in een smoothfs-mount wordt geïdentificeerd via object-metadata.
2. Bestandstoegang wordt afgehandeld vanaf de huidige lower file.
3. Planner evalueert heat en intentie; indien beleid dit toelaat, wordt een
   movement plan gemaakt.
4. Worker voert copy/verify/cutover state transitions uit.
5. Kernel swapt per-object lower pad bij `cutover` en de bron wordt opgeschoond.

## Control plane contract punten

Kernel en userspace delen deze opdrachtfamilies:

- register/inspect/reprobe pool
- reconcile/quiesce/reconciliëren met reden
- move planning en cutover
- policy push en tier-down controls

Deze staan in `uapi.go` en `client.go` en zijn geïmplementeerd in netlink handlers.

## Recovery en foutenafhandeling

- niet-terminale moves worden deterministisch afgedraaid bij opstart
- indien cutover in successtates is, herstelt recovery naar `placed` op bestemming
- bij falen pre-cutover wordt teruggerold naar stabiele bronstate
- actieve-LUN moves behouden expliciet quiesced target state en vereisen expliciete
  re-pin + resume gating
- movement logs registreren terminale en failed states voor forensische analyse

## Waarom dit belangrijk is voor SmoothNAS

- operaties overleven service restarts zonder losgezongen movement-state
- actieve beweging is expliciet en te auditen i.p.v. impliciet achtergrondgedrag
- protocolservices (NFS/SMB/iSCSI) blijven geïntegreerd via stabiele pins,
  movement events en recovery-voorwaarden

## Build- en testpaden

Deze laag wordt via DKMS gebouwd in `smoothfs/debian` binnen deze boom.

Kernel-scripts:

- `smoothfs/test/kernel_upgrade.sh`
- `smoothfs/test/module_signing.sh`
- protocol- en movement suites in `smoothfs/test/*.sh`

## Verder lezen

- [`uapi_smoothfs.h`](uapi_smoothfs.h): netlink schema
- [`controlplane/contract.go`](../controlplane/contract.go): interne movement-contracten
- [`../docs/architecture.md`](../docs/architecture.md): overzicht over cross-component architectuur
