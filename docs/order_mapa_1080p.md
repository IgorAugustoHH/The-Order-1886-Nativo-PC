# The Order em 1080p: mapa dos alvos da cena (captura run119, frame_1791907385, 151 passes)

Base para os modos do DLSS: tudo o que precisa encolher (a cena 3D até o resolve) e tudo o que
precisa continuar em 1080p (pós-processamento e interface).

## Alvos 1920x1080 por função

| Endereço | Formato | Papel (pela ordem de uso) |
| --- | --- | --- |
| 0x104d4d0000 | D32 2x MSAA | profundidade da cena (prepass #2, opacos #50, transparentes #60-#67) |
| 0x1053e00000 | A2B10G10R10 2x MSAA | normais do prepass (#2). O mesmo endereço é reusado como RGBA16F 1080p (#70) |
| 0x104bc98000 | A2B10G10R10 | velocidade (prepass #1-#9); entrada do resolve e do motion blur (#71, #77) |
| 0x104b610000 | R16Uint | ID/máscara do prepass (#1, #2); lida pelas listas de luz (#26) |
| 0x1055100000 | RGBA16F 2x MSAA | cor HDR da cena (#50-#68); entrada do resolve |
| 0x104c4a0000 | RGBA16F | histórico do TAA (entrada do resolve) |
| 0x1049c00000 | RGBA16F | saída do resolve: aqui o DLSS escreve 1080p |
| 0x104abf8000 | R8G8 | AO (compute #28), lido pelos opacos |
| 0x104ba50000 | R8 | máscara (sombra/luz?) lida por quase todos os passes da cena |
| 0x1057340000 | RGBA16F | gerado em #14, lido pelos opacos |
| 0x104e5e8000 | D32 | profundidade sem MSAA (#9). **Também lida no pós** (#78, #85, #118: DoF/névoa/tonemap) |
| 0x104ef20000 | D32 | profundidade de outro passe (#12), lida no compute #32 |
| 0x104f858000 | D32 | profundidade resolvida (#5). **Também usada pela interface** (#120-#146) |
| 0x1056a00000 | RGBA16F | pós-processamento (#85 em diante): fica em 1080p |
| 0x10588e0000 / 0x10550f8000 | RGBA8 / A2B10G10R10 | tonemap e imagem final: ficam em 1080p |

Os computes da cena que leem profundidade em coordenadas de tela são #26 (listas de luz), #27, #28
(AO), #30 e #32. Todos têm de ler a versão reduzida, coerente com a cena reduzida.

## Riscos encontrados

1. **Endereços reaproveitados no mesmo quadro:**
   - `0x1053e00000` serve de normal MSAA e de RGBA16F;
   - `0x10550f8000` (imagem final) é lido antes pelos computes #27/#28, como outro recurso.

   Reduzir por endereço quebraria essas reutilizações. A redução tem de seguir a imagem
   (formato + MSAA + passe), não só o endereço.
2. **Profundidades que cruzam a fronteira cena/pós:**
   - `0x104e5e8000` é usado pelo DoF e pelo tonemap;
   - `0x104f858000` é usado pela interface.

   Precisam de uma versão 1080p reconstruída depois da cena: reamostragem da profundidade
   reduzida, como o sistema herdado já faz com `NativeAccess`.
3. **MSAA 2x:** o `SceneTargets` herdado aceita só 1920x1080 sem MSAA (`Eligible`) e tem de ser
   estendido para alvos 2x.

## Plano

1. Estender `SceneTargets::Eligible` para os alvos acima, incluindo MSAA 2x, com a lista fechada
   de formatos. Ativar entre o início do prepass e o resolve.
2. Viewport e scissor da cena escalados (o sistema herdado já faz isso para o Bloodborne).
3. No resolve, o `OrderDlss` usa as entradas reduzidas e escreve a saída 1920x1080 nos modos
   Qualidade, Equilibrado, Desempenho e Ultra Desempenho.
4. Depois do resolve, as profundidades que cruzam a fronteira são reamostradas para 1080p
   (`NativeAccess`).
5. Validar o foco da mira, que lê a profundidade no centro (PS f77d3cf1, #7), e o DoF.
