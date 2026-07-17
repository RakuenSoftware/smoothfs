# Changelog

## [0.2.18](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.17...v0.2.18) (2026-07-17)


### Bug Fixes

* **movement:** refuse cutover to an incomplete destination copy ([#170](https://github.com/RakuenSoftware/smoothfs/issues/170)) ([9f763da](https://github.com/RakuenSoftware/smoothfs/commit/9f763da3d09e3ff04831839df59da07a067a5967))

## [0.2.17](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.16...v0.2.17) (2026-07-14)


### Bug Fixes

* survive setattr/xattr/acl on unlinked-but-open files + vfs_mkdir error-path lock leaks ([#168](https://github.com/RakuenSoftware/smoothfs/issues/168)) ([4d8b44a](https://github.com/RakuenSoftware/smoothfs/commit/4d8b44a7b371d36d138a552ea92e37bf5ad517c3))

## [0.2.16](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.15...v0.2.16) (2026-07-13)


### Bug Fixes

* **inode:** take the pending OID from the writeback queue instead of draining it ([#166](https://github.com/RakuenSoftware/smoothfs/issues/166)) ([53262e0](https://github.com/RakuenSoftware/smoothfs/commit/53262e0b975b3cb2a8b5483c2e242ce98fe1049a))

## [0.2.15](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.14...v0.2.15) (2026-07-13)


### Bug Fixes

* **compat:** adapt rename to the 6.19 VFS directory-locking rework (build 6.19/7.x) ([4b61ce6](https://github.com/RakuenSoftware/smoothfs/commit/4b61ce684fe19a4e83243beab1c9428ead9965ae))
* **inode:** drain OID writeback before re-minting so file identity is stable ([3031244](https://github.com/RakuenSoftware/smoothfs/commit/303124469a9099f7c4476d61aa148aaec037b3f5))

## [0.2.14](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.13...v0.2.14) (2026-07-05)


### Bug Fixes

* **placement:** add forget_lower sysfs to reclaim out-of-band-removed tier copies ([#161](https://github.com/RakuenSoftware/smoothfs/issues/161)) ([3f19356](https://github.com/RakuenSoftware/smoothfs/commit/3f19356ed5c726882bdaae211f649b1a70781c1c))

## [0.2.13](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.12...v0.2.13) (2026-06-12)


### Bug Fixes

* **lookup:** scan all tiers in the across-tiers fallback (readdir/lookup coverage parity) ([#159](https://github.com/RakuenSoftware/smoothfs/issues/159)) ([6def505](https://github.com/RakuenSoftware/smoothfs/commit/6def505306a3fdbed3065d9f0d938e5624529a80))

## [0.2.12](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.11...v0.2.12) (2026-06-11)


### Bug Fixes

* **rename:** complete cross-directory cross-tier renames instead of EXDEV ([#157](https://github.com/RakuenSoftware/smoothfs/issues/157)) ([dd93fb9](https://github.com/RakuenSoftware/smoothfs/commit/dd93fb99e2001d423ccbb805dc2af7e47d7eab17))

## [0.2.11](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.10...v0.2.11) (2026-06-06)


### Bug Fixes

* **rename:** complete same-directory cross-tier renames instead of EXDEV ([#153](https://github.com/RakuenSoftware/smoothfs/issues/153)) ([6dd2e1f](https://github.com/RakuenSoftware/smoothfs/commit/6dd2e1f9b09e5f4dad20dd79fc6c7e3420a558eb))

## [0.2.10](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.9...v0.2.10) (2026-06-06)


### Bug Fixes

* **placement:** create_policy=fastest places on fastest tier with space ([#151](https://github.com/RakuenSoftware/smoothfs/issues/151)) ([fddaf68](https://github.com/RakuenSoftware/smoothfs/commit/fddaf68e301fcb5ed9db3643562a319fed9b59d1))

## [0.2.9](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.8...v0.2.9) (2026-06-06)


### Performance Improvements

* lazy mount-replay instantiation ([#149](https://github.com/RakuenSoftware/smoothfs/issues/149)) ([19aecc8](https://github.com/RakuenSoftware/smoothfs/commit/19aecc842532167952762c23318fc1ecd73a0035))

## [0.2.8](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.7...v0.2.8) (2026-06-06)


### Performance Improvements

* parallelize mount-time placement replay ([#147](https://github.com/RakuenSoftware/smoothfs/issues/147)) ([371cf58](https://github.com/RakuenSoftware/smoothfs/commit/371cf58110111e8e5b2402e0ab2b914cab6d25ee))

## [0.2.7](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.6...v0.2.7) (2026-06-06)


### Bug Fixes

* run all backing ops with privileged creds and present a uniform owner ([#145](https://github.com/RakuenSoftware/smoothfs/issues/145)) ([5f9e4c1](https://github.com/RakuenSoftware/smoothfs/commit/5f9e4c1e8beb1736e0bf581218b62bf94e2a5f00))

## [0.2.6](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.5...v0.2.6) (2026-06-06)


### Bug Fixes

* own materialized spill-tier dirs to match the canonical tier ([#143](https://github.com/RakuenSoftware/smoothfs/issues/143)) ([c0c1bf9](https://github.com/RakuenSoftware/smoothfs/commit/c0c1bf9b1f40682f06f5ccd0932d7e90ca97e52b))

## [0.2.5](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.4...v0.2.5) (2026-06-06)


### Bug Fixes

* move spill-tier copies when renaming a multi-tier directory ([#141](https://github.com/RakuenSoftware/smoothfs/issues/141)) ([9aec0ef](https://github.com/RakuenSoftware/smoothfs/commit/9aec0efb4a8b84a842ad93123228e94b6f72006a))

## [0.2.4](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.3...v0.2.4) (2026-06-06)


### Bug Fixes

* purge spill-tier copies when rmdir'ing a canonically-resolved dir ([#139](https://github.com/RakuenSoftware/smoothfs/issues/139)) ([c451706](https://github.com/RakuenSoftware/smoothfs/commit/c451706796bd713b01640934a20be59981bd2535))

## [0.2.3](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.2...v0.2.3) (2026-06-05)


### Bug Fixes

* detect removed dir alias by lower nlink, not d_really_is_negative ([#137](https://github.com/RakuenSoftware/smoothfs/issues/137)) ([67fcd64](https://github.com/RakuenSoftware/smoothfs/commit/67fcd64698541946edc38054cccb2321b3c6f90c))

## [0.2.2](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.1...v0.2.2) (2026-06-05)


### Bug Fixes

* don't resurrect a removed multi-tier directory from a stale alias ([#135](https://github.com/RakuenSoftware/smoothfs/issues/135)) ([d07e41d](https://github.com/RakuenSoftware/smoothfs/commit/d07e41d149692ff1790461281beb8d892d3ca174))

## [0.2.1](https://github.com/RakuenSoftware/smoothfs/compare/v0.2.0...v0.2.1) (2026-06-05)


### Performance Improvements

* O(1) rel_path lookups via path_map rhashtable ([#134](https://github.com/RakuenSoftware/smoothfs/issues/134)) ([27f3468](https://github.com/RakuenSoftware/smoothfs/commit/27f346823b22b208633f045a70403acab6285876))

## [0.2.0](https://github.com/RakuenSoftware/smoothfs/compare/v0.1.0...v0.2.0) (2026-06-04)


### Features

* accept bcachefs lower filesystems ([befc0d9](https://github.com/RakuenSoftware/smoothfs/commit/befc0d9f798c12e462c92d215b65f42f8e758051))
* add high-water create policy ([107d187](https://github.com/RakuenSoftware/smoothfs/commit/107d18783219ed29ff0b2d53ac74e4888abe8fb2))
* add high-water create policy ([6012b15](https://github.com/RakuenSoftware/smoothfs/commit/6012b152c9ea17dee02e555f537373f69b229010))


### Bug Fixes

* address smoothfs audit findings ([b8ab1f7](https://github.com/RakuenSoftware/smoothfs/commit/b8ab1f7d6c91c318aca4208f4171d5f85c62d592))
* avoid blocking mounts on cold path index ([e881bcf](https://github.com/RakuenSoftware/smoothfs/commit/e881bcfd15f0f6af7774abf48ef873f82413766b))
* avoid blocking smoothfs mounts on cold path index ([c753635](https://github.com/RakuenSoftware/smoothfs/commit/c75363594c899f5a655c47ab97d1cdd1842050e8))
* close service client on cancellation ([5208456](https://github.com/RakuenSoftware/smoothfs/commit/52084565fe9a7b4d97a1ecc865657ae05499b8da))
* close service client on cancellation ([7ef71a7](https://github.com/RakuenSoftware/smoothfs/commit/7ef71a7e8bb2bc32f49ddf2831612dbc399fba13))
* complete remaining audit remediations ([6d214bd](https://github.com/RakuenSoftware/smoothfs/commit/6d214bd4ac2f9dead835b31cf950f6606fcfeba0))
* declare materialized parent helper before use ([31a694c](https://github.com/RakuenSoftware/smoothfs/commit/31a694c8ed5f2896ad8416a620d2802f99031642))
* guard movement cutover with write sequence ([ca29a86](https://github.com/RakuenSoftware/smoothfs/commit/ca29a86e43ca0739a997e6e11b3fcdfed92d2ea1))
* include statfs for kernel module build ([84714d0](https://github.com/RakuenSoftware/smoothfs/commit/84714d01c154d9884726de46765a18b611d7ddcd))
* **inode:** drop placement identity on unlink/rmdir to stop zombie dirs ([#128](https://github.com/RakuenSoftware/smoothfs/issues/128)) ([b76f7d7](https://github.com/RakuenSoftware/smoothfs/commit/b76f7d761d8842212ba83eb031669a4a852499f6))
* **pools:** set TimeoutSec=infinity on generated mount unit ([26df046](https://github.com/RakuenSoftware/smoothfs/commit/26df046ea35402807c391ee3ae974089a2339857))
* **pools:** set TimeoutSec=infinity on generated mount unit ([c2a32b3](https://github.com/RakuenSoftware/smoothfs/commit/c2a32b324449c74e0779e665809f77cc7b332390))
* **smoothfs:** refuse cross-tier remap to avoid lower-fs NULL deref ([#127](https://github.com/RakuenSoftware/smoothfs/issues/127)) ([cfcc948](https://github.com/RakuenSoftware/smoothfs/commit/cfcc948d528bf693adbfbe2f3c360f7782b36ba0))


### Performance Improvements

* **placement:** add path index to skip tier scan on remount ([50a814b](https://github.com/RakuenSoftware/smoothfs/commit/50a814bae8894832d2c230b52b279a981807d131))
* **placement:** path index to skip tier scan on remount ([28d68d8](https://github.com/RakuenSoftware/smoothfs/commit/28d68d87187011fe20ddf60b03746cbe53fd5eee))
* **placement:** path index to skip tier scan on remount ([28d68d8](https://github.com/RakuenSoftware/smoothfs/commit/28d68d87187011fe20ddf60b03746cbe53fd5eee))
