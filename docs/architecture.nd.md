# smoothfs architectuur

Dit is een korte Nederlandstalige samenvatting. De uitgebreide en canonieke
architectuurreferentie staat in [`architecture.md`](architecture.md).

Dit document beschrijft hoe de onderdelen in een SmoothNAS-implementatie samen
spelen.

## 1) Systeemgrens

smoothfs is verdeeld over drie werkdomeinen:

1. **Kernel filesystem driver** (`src/smoothfs`)
2. **Userspace control plane** (`controlplane`)
3. **Appliance-integratie** (buiten deze repo, nu `smoothnas`)

De kerneldriver beheert bestandsresolutie en movement state transitions.
De userspace controlplane beheert beleid, planningfrequentie en orchestratie.
De appliance beheert UI/API entry points en operationele waarborgen.

## 2) Kerncomponenten

### Kerneldriver (`src/smoothfs`)

- Biedt stacked filesystem mount type `smoothfs`.
- Beheert object identity en per-object metadata in persistente xattrs.
- Exporteert lifecycle events en command families via generic netlink.
- Implementeert copy/cutover-ready movement primitieve.
- Ondersteunt write staging, range staging en movement-gating gedrag voor bovenlagen.

### controlplane (`controlplane`)

- Detecteert gemounte pools en ververst lokale cache.
- Berekent movement candidates op basis van EWMA heat + policy beperkingen.
- Geeft geplande bewegingen door aan de kernel (`MovePlan`, `MoveCutover`).
- Verwerkt retries en terminale state transitions voor niet-terminale moves bij opstart.
- Persisteert:
  - pool- en objectstatus
  - movement history
  - movement policies en operator reconcile redenen

### SmoothNAS integratievlak

- Gebruikt de `tierd` API-oppervlakte en gedeelde auth/autorisatie flows.
- Exposeert quiesce/reconcile, poolbeheer en movement logs in UI en CLI.
- Verwerkt protocol-specifieke integratiedetails:
  - SMB VFS module loading en event handling
  - iSCSI fileio pin gedrag (`trusted.smoothfs.lun`)
  - installatie / upgrade / rollback scenario's

## 3) Normale movement flow

```text
Heat sampling / policy evaluatie
             │
             ▼
      controlplane planner
             │ emits plan
             ▼
    controlplane worker (copy + verify)
             │
      kernel MoveCutover
             │
       bron cleanup + state log
```

Belangrijk controlepunt: movement state is expliciet en observeerbaar.

## 4) Active-LUN flow

Active-LUN backing files lopen niet door de normale planner.
Ze gebruiken een operator-gecontroleerde flow:

- doel (target) quiescen/drainen
- bronpin wissen
- voorbereid movement plan met re-pin intent
- cutover + verificatie van destination pin
- target hervatten

Deze flow voorkomt dat een actief LUN onbeschermd blijft voor I/O clients.

## 5) Failure en restart gedrag

- Elke in-flight beweging wordt bij opstart gereconstrueerd.
- Cutover-partiële states worden deterministisch afgerond of teruggedraaid.
- Pinstatus wordt opnieuw toegepast waar nodig voor hervatting van clients.
- Movement logs blijven beschikbaar voor foutanalyse en escalatie.

## 6) Observability

Operationele zichtbaarheid omvat:

- movement transitions in `smoothfs_movement_log`
- reconcile/quiesce acties
- tier en policy configuratie in `control_plane_config`
- runtime kernel health en scripts

## 7) Security en lifecycle

- Module signing en MOK helper scripts horen bij DKMS packaging.
- Kernel- en userspace-versies zijn uitgelijnd via support matrix.
- Upgrade/rollback gedrag leunt op per-kernel DKMS trees en expliciete health checks.

## 8) Lees verder

- Operator uitvoering: [`smoothfs-operator-runbook.nd.md`](smoothfs-operator-runbook.nd.md)
- Platform grenzen: [`smoothfs-support-matrix.nd.md`](smoothfs-support-matrix.nd.md)
- Runtime technische details: [`../src/README.nd.md`](../src/README.nd.md)
