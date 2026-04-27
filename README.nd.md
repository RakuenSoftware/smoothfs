# smoothfs

smoothfs is de opslag-engine achter SmoothNAS tiering. Het geeft SmoothNAS één
centrale opslaglaag waar snelle en langzame data samen werken, terwijl bestanden
automatisch tussen tiers worden verplaatst.

Waarvoor mensen het gebruiken:

- hou actieve bestanden op SSD/NVMe en koude bestanden op langzamere media
- behoud vertrouwde NFS/SMB/iSCSI-gedrag terwijl opslagplaatsing wordt geoptimaliseerd
- verlaag kosten zonder NAS-workflows te veranderen
- veiligere data-beweging met expliciete statusovergangen en herstelpaden

Voor SmoothNAS-operators betekent dat:

- betere prestaties waar dat nodig is
- eenvoudiger opslaggroei op gemengd hardware
- voorspelbaar gedrag omdat actieve-LUN-verplaatsingen en recovery expliciet zijn

## Waarom nuttig

- **Eén namespace, meerdere mediatypes**: één mountpad en één weergave voor eindgebruikers.
- **Beleidsgedreven**: heat + beleid bepalen beweging, met behoud van POSIX-correctheid.
- **Productie-veilig**: quiesce/reconcile, movement logs en expliciete actieve-LUN-workflows voorkomen verrassingsgedrag.
- **Geïntegreerde pakketpaden**: kernelmodule, userspace daemon en scripts voor ondersteuning
  worden als Debian-artifacts geleverd voor SmoothNAS.

## Voor SmoothNAS

SmoothNAS moet deze repository als externe component gebruiken in plaats van de
implementatie te dupliceren.

- kernel filesystem + movement engine: `src/smoothfs`
- userspace control-plane: `controlplane`
- API-contracten: [`client.go`](client.go), [`events.go`](events.go),
  [`uapi.go`](uapi.go), [`pools.go`](pools.go)
- operator docs en gefaseerde uitrol: [`docs`](docs)

## Documentatie

Engels:

- [support matrix](docs/smoothfs-support-matrix.md)
- [operator runbook](docs/smoothfs-operator-runbook.md)
- [architectuur](docs/architecture.md)
- [technische README in `src`](src/README.md)

Nederlands:

- [ondersteuningsmatrix](docs/smoothfs-support-matrix.nd.md)
- [runbook](docs/smoothfs-operator-runbook.nd.md)
- [architectuur](docs/architecture.nd.md)
- [technische README in `src`](src/README.nd.md)

## Verificatie

```bash
go test ./...
```

Runtime tests voor kernel/packaging/Samba en iSCSI bevinden zich onder
[`src/smoothfs/test`](src/smoothfs/test) en draaien op een Linux-host met de juiste
kernel-, bestandsysteem-, Samba- en iSCSI-vereisten.

## Repository-indeling

- `src/smoothfs`: kernelmodule, DKMS, Samba VFS integratie, en test harnesses
- `controlplane`: planner/worker/recovery servicelogica
- `docs`: operator en ontwerpd documentatie
- `tierd` uit SmoothNAS consumeert deze contracten
