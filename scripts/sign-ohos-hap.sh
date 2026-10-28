#!/usr/bin/env bash
#
# Sign a HarmonyOS HAP with locally held debug/release material.
#
# Required environment:
#   HAP_SIGN_KEY      path to the .p12 keystore
#   HAP_SIGN_CERT     path to the .cer certificate
#   HAP_SIGN_PROFILE  path to the .p7b provisioning profile
#   HAP_SIGN_PASSWORD keystore/key password
# Optional:
#   HAP_SIGN_ALIAS    key alias (default: sirildebug)
#   OHOS_SDK          HarmonyOS SDK root (default: DevEco default SDK)
#   JAVA              java executable
#
#   scripts/sign-ohos-hap.sh entry-default-unsigned.hap SIRIL-signed.hap
#
set -euo pipefail

IN_HAP="${1:?usage: sign-ohos-hap.sh <unsigned.hap> <signed.hap>}"
OUT_HAP="${2:?usage: sign-ohos-hap.sh <unsigned.hap> <signed.hap>}"

: "${HAP_SIGN_KEY:?HAP_SIGN_KEY is required}"
: "${HAP_SIGN_CERT:?HAP_SIGN_CERT is required}"
: "${HAP_SIGN_PROFILE:?HAP_SIGN_PROFILE is required}"
: "${HAP_SIGN_PASSWORD:?HAP_SIGN_PASSWORD is required}"

SDK="${OHOS_SDK:-/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony}"
JAVA_BIN="${JAVA:-java}"

"$JAVA_BIN" -jar "$SDK/toolchains/lib/hap-sign-tool.jar" sign-app \
  -keyAlias "${HAP_SIGN_ALIAS:-sirildebug}" \
  -signAlg SHA256withECDSA \
  -mode localSign \
  -appCertFile "$HAP_SIGN_CERT" \
  -profileFile "$HAP_SIGN_PROFILE" \
  -inFile "$IN_HAP" \
  -keystoreFile "$HAP_SIGN_KEY" \
  -outFile "$OUT_HAP" \
  -keyPwd "$HAP_SIGN_PASSWORD" \
  -keystorePwd "$HAP_SIGN_PASSWORD"

echo "signed: $OUT_HAP"
