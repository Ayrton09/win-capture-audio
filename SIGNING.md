# Firma de código / Code signing

El DLL se firma con [cmake/sign-plugin.ps1](cmake/sign-plugin.ps1):

```
powershell -File cmake\sign-plugin.ps1 -Thumbprint <huella-del-cert> -Path build\RelWithDebInfo\win-capture-audio.dll
```

Firma SHA-256 con timestamp RFC-3161 (la firma sigue siendo válida cuando el certificado
expira). Acepta un certificado del almacén `CurrentUser\My` (`-Thumbprint`, recomendado;
compatible con tokens de hardware) o un archivo `.pfx` (`-PfxPath`).

## Estado actual

El repositorio incluye el pipeline funcionando con un **certificado autofirmado de
desarrollo** (`CN=win-capture-audio dev signing`). Eso prueba la integridad del archivo,
pero **no** evita falsos positivos de antivirus ni avisos de SmartScreen en otras máquinas:
para eso la firma debe encadenar a una CA en la que Windows confía.

## Para distribuir de verdad, en orden de recomendación

1. **Certum – Open Source Code Signing** (~35 €/año + lector de tarjeta la primera vez):
   el más usado por desarrolladores de plugins de OBS; requiere verificación de identidad
   individual. Al recibirlo: instalar su tarjeta/token, y firmar con `-Thumbprint` (el cert
   aparece en `CurrentUser\My`).
2. **Azure Trusted Signing** (~10 US$/mes): verificación individual u organizacional,
   integración por CLI; buena opción si ya usás Azure.
3. **SignPath.io – plan open source** (gratis; la ruta elegida para este proyecto): firma en
   la nube integrada al CI. El workflow ya está preparado en
   `.github/workflows/build.yml` (job `sign`, corre al taggear). Pasos pendientes, una vez
   que el repo esté público en GitHub:
   1. Solicitar el plan OSS en <https://about.signpath.io/product/open-source> indicando la
      URL del repo (aprueban proyectos OSS reales; tarda unos días).
   2. En la organización SignPath que te crean: proyecto `win-capture-audio`, signing policy
      `release-signing`, y un Artifact Configuration para el zip del artefacto de CI
      (deep-sign de `bin/64bit/win-capture-audio.dll`).
   3. En GitHub → Settings → Secrets and variables → Actions, crear
      `SIGNPATH_API_TOKEN` y `SIGNPATH_ORG_ID` con los valores que da SignPath.
   4. Taggear una release: el job `sign` sube el artefacto, SignPath lo firma con su
      certificado OSS (confiable para Windows) y el workflow publica el artefacto firmado.
4. **OV clásico** (Sectigo/DigiCert, ~70–400 US$/año): innecesariamente caro para esto.

Con cualquiera de ellos, el único cambio es el valor de `-Thumbprint`.

## Además de firmar (gratis y efectivo contra falsos positivos)

- Enviar el DLL a los portales de falsos positivos: [Microsoft](https://www.microsoft.com/en-us/wdsi/filesubmission),
  [Kaspersky](https://opentip.kaspersky.com/), y los de cualquier AV que lo marque.
- Publicar los binarios como GitHub Release con checksums (`Get-FileHash -Algorithm SHA256`)
  en las notas: la reputación de descarga es parte de la heurística de SmartScreen/AV.
- La reputación tarda: incluso con certificado OV, las primeras semanas puede haber avisos
  hasta que el binario acumula descargas.
