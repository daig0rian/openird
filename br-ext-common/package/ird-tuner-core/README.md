# ird-tuner-core

OpenIRD の **チューナ ABI 契約**。px4_drv 互換 chardev（生 TS を読める口）を提供する
ベンダ非依存の骨格。機種別復調ドライバはこの ABI の上に実装する。

- 段階 C（Milestone 2）で clean-room 実装。根拠は `docs/specs/` の chardev 契約節。
- スコープは**生 TS の搬送のみ**。復号（MULTI2 等）は契約の上位＝OpenIRD 外。
