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
3. **SignPath.io – plan open source** (gratis para proyectos OSS con CI público): firma en
   la nube integrada a GitHub Actions; requiere que el repo sea público y aprobación.
4. **OV clásico** (Sectigo/DigiCert, ~70–400 US$/año): innecesariamente caro para esto.

Con cualquiera de ellos, el único cambio es el valor de `-Thumbprint`.

## Además de firmar (gratis y efectivo contra falsos positivos)

- Enviar el DLL a los portales de falsos positivos: [Microsoft](https://www.microsoft.com/en-us/wdsi/filesubmission),
  [Kaspersky](https://opentip.kaspersky.com/), y los de cualquier AV que lo marque.
- Publicar los binarios como GitHub Release con checksums (`Get-FileHash -Algorithm SHA256`)
  en las notas: la reputación de descarga es parte de la heurística de SmartScreen/AV.
- La reputación tarda: incluso con certificado OV, las primeras semanas puede haber avisos
  hasta que el binario acumula descargas.
