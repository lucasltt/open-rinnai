# API local do aquecedor

`192.168.1.50` é um endereço ilustrativo em todos os exemplos. Substitua pelo IP atribuído ao seu ESP32.

A API v1 está disponível no firmware **1.1.0 ou posterior**. A documentação servida pelo módulo fica em **[192.168.1.50/api/docs](http://192.168.1.50/api/docs)**, e a descrição importável em **[OpenAPI JSON](http://192.168.1.50/api/openapi.json)**. A base é `http://192.168.1.50/api/v1`; a versão do contrato é independente da versão do firmware. Confira a versão instalada no painel. Os arquivos locais `api/index.html` e `api/openapi.json` contêm a mesma documentação.

A API HTTP está sem autenticação na rede local, conforme a configuração do painel. Um visor, script ou integração deve alcançar o ESP nessa rede. Os endpoints existentes em `/rinnai/api/` continuam atendendo o painel com formulários URL-encoded; novos consumidores podem usar a v1 em JSON.

O firmware **1.2.0** acrescenta `GET /sessions` para sincronizar usos concluídos com identidade durável, mantendo o contrato **API v1**. Confira `capabilities.completed_sessions` e `endpoints.sessions` em `GET /capabilities` antes de usar esse recurso. Os demais endpoints continuam compatíveis com o firmware 1.1.0.

## Endpoints

| Método e caminho após `/api/v1` | Uso |
|---|---|
| `GET /` ou `GET /capabilities` | Recursos suportados, limites, unidades, links e intervalos sugeridos |
| `GET /state` | Estado completo: leituras, uso atual, consumos, configurações, avisos e diagnóstico |
| `GET /telemetry` | Leituras e sua atualidade, para um visor remoto |
| `GET /settings` | Tarifas, três favoritos e configuração dos avisos |
| `GET /control` | Resultado do último comando HTTP de temperatura |
| `GET /history` | Até 720 pontos do gráfico e 24 usos concluídos |
| `GET /sessions` | Desde 1.2.0: até 24 usos concluídos de um checkpoint durável, com IDs estáveis; sem pontos do gráfico |
| `GET /periods` | Até 31 dias e 12 meses de consumo e duração |
| `GET /diagnostics` | Memória, armazenamento, inicializações e registro da última falha |
| `GET /health` | `200` com comunicação/armazenamento disponíveis; `503` em estado inicial ou degradado |
| `POST /temperature` | Solicitar temperatura absoluta entre 35 e 50 °C |
| `POST /favorite` | Aplicar a temperatura de um favorito, posição 1 a 3 |
| `POST /settings` | Substituir tipo de gás e as duas tarifas |
| `POST /preferences` | Substituir os três favoritos e as opções de aviso |
| `POST /control/cancel` | Impedir novos passos de um ajuste em andamento |
| `POST /device/restart` | Reiniciar somente o módulo ESP32 |

POSTs da v1 exigem `Content-Type: application/json`, corpo de até **2048 bytes** e os campos exatos de cada operação. Tipos incorretos, campos desconhecidos e objetos incompletos são recusados. O contrato desta implementação usa **GET e POST**; PUT e PATCH não são implementados.

## Leitura, unidades e atualidade

O cache de estado é atualizado aproximadamente uma vez por segundo. Para o visor, consulte `/telemetry` a cada **2 segundos**. Consulte histórico, usos concluídos (`/sessions`), períodos e diagnóstico a cada **10 segundos ou mais**. Fazer mais consultas não acelera as leituras físicas: os oito registradores são consultados em sequência, em momentos diferentes.

| Campo | Unidade ou significado |
|---|---|
| `online` | Comunicação recente com o aquecedor |
| `heating` | Chama informada, com comunicação disponível |
| `inlet_c`, `outlet_c` | Temperaturas de entrada e saída, em °C |
| `setpoint_c`, `target_c` | Temperatura confirmada e alvo do controlador, em °C |
| `flow_l_min` | Vazão, em L/min |
| `power_kcal_min`, `power_kw` | Potência informada e conversão para kW |
| `readings.*.age_s`, `readings.*.stale` | Idade e vencimento de cada leitura |
| `error_code` | Código de erro recente ou `null` se desconhecido |
| `sampled_at_epoch_s` | Horário UTC da atualização do cache, ou `null` sem relógio válido |
| `sampled_at_uptime_s` | Instante da atualização do cache em segundos desde o boot |

Os metadados `sampled_at_*` pertencem à atualização do cache. Eles não tornam os registradores físicos uma amostra simultânea. Use também a idade específica de cada leitura.

**`null` significa desconhecido, não zero.** Em `/telemetry`, `setpoint_c` torna-se `null` quando `readings.setpoint.stale` é verdadeiro. Em `/state` e `/control`, esse número pode ser conservado como valor histórico depois de vencer: só o apresente como atual se `online` for verdadeiro e `readings.setpoint.stale` for falso. Entrada/saída/vazão/potência/chama têm janela de 30 s; setpoint e erro usam 15 s. O código de erro desconhecido deve ser apresentado como desconhecido.

Para validar a atualidade de um valor recebido em `/control`, consulte os indicadores de `/state` ou `/telemetry`; a resposta de controle não duplica todas as idades das leituras.

As contabilidades usam `water_l` em litros, `energy_kwh` em kWh e `heating_s` em segundos. `gas` representa m³ de GN ou kg de GLP, conforme `settings.gas_unit`. Gás e custo são estimativas calculadas com a potência informada e as tarifas atuais. `gas_cost` e `water_cost` separam as estimativas em R$ no total acumulado, uso atual e resumos de períodos. Cada parcela é `null` quando sua tarifa não está informada (valor zero na configuração). `cost` soma somente as parcelas disponíveis e é `null` sem nenhuma tarifa; com apenas uma tarifa configurada, `cost_complete` é falso. Um consumo zero com tarifa configurada gera custo zero válido. Não há medidor físico de gás nem livro de tarifas históricas.

## Ajustar e acompanhar

Exemplo de corpo:

```json
{"target":38}
```

O alvo é absoluto, inteiro, entre **35 e 50 °C**. O controlador consulta a temperatura física antes e depois de cada passo necessário. Uma resposta `202 Accepted` confirma apenas que o pedido entrou na fila:

```json
{"accepted":true,"boot_id":18,"command_id":1,"target":38,"status_url":"/api/v1/control"}
```

Após essa resposta, consulte `/control`. Confira o par **`boot_id` + `command_id`** antes de associar o resultado ao seu pedido. O módulo conserva somente o último comando HTTP; um novo comando pode substituir esse registro, e um reinício muda o boot. Não existe histórico permanente de comandos.

| `result` | Interpretação |
|---|---|
| `idle` | Nenhum comando HTTP acompanhado neste boot |
| `queued` | Aceito, aguardando o loop de controle |
| `running` | Consulta/ajuste em andamento |
| `confirmed` | Alvo confirmado pela resposta física |
| `failed` | Pedido recusado ou interrompido sem confirmação de sucesso |
| `cancelled` | Pedido cancelado; confira também `uncertain` |

`status` é uma mensagem para pessoas. Para decisões automáticas, use `result`, `uncertain`, os IDs e a atualidade de `setpoint_c`. Há somente uma vaga de controle; com uma operação em andamento, outro pedido retorna `409`. Se a conexão cair após um POST, consulte o estado antes de decidir por um novo pedido. Não repita automaticamente um incremento/decremento incerto.

`POST /favorite` recebe `{"slot":1}`. As posições são 1, 2 e 3, na ordem de `GET /settings`. É um comando explícito que solicita a temperatura do favorito imediatamente; o botão de favorito do painel visual continua tendo seu fluxo próprio de selecionar e depois aplicar.

`POST /control/cancel` recebe `{}`. Retorna `202` se há operação a cancelar e `200` se já está ocioso. Em `/control`, `cancel_requested=true` indica o pedido de cancelamento ainda aguardando processamento. Cancelar impede os próximos passos; não desfaz um passo que já foi enviado. Um novo ajuste explícito sempre começa por leitura física.

## Configurações e manutenção

`POST /settings` recebe os três campos abaixo. As tarifas ficam entre 0 e 100000; zero significa não informada. GN usa R$/m³ e GLP usa R$/kg. A água usa R$/m³.

```json
{"gas_type":"GN","gas_tariff":0,"water_tariff":0}
```

`POST /preferences` recebe todos os favoritos e avisos:

```json
{
  "favorites": [
    {"name":"Banho","temp_c":38},
    {"name":"Conforto","temp_c":39},
    {"name":"Cozinha","temp_c":40}
  ],
  "alerts": {
    "long_use_enabled":true,
    "flow_enabled":true,
    "error_enabled":true,
    "offline_enabled":true,
    "long_use_min":15,
    "continuous_flow_min":20,
    "offline_seconds":30
  }
}
```

Há exatamente três favoritos. Cada nome aceita até **31 bytes UTF-8**, após remover espaços ASCII das extremidades, sem nome vazio nem caracteres de controle. A temperatura vai de 35 a 50 °C. As quatro opções de aviso são booleanos JSON; os dois limites de uso vão de 1 a 180 minutos e o limite sem resposta vai de 10 a 600 segundos, todos inteiros.

Esses POSTs também são assíncronos. Após `202`, leia **`GET /settings`** e compare os valores para confirmar a persistência. Salvar um favorito não muda a temperatura atual. Os avisos são condições locais consultáveis pela API; não significam notificações push no celular.

`POST /device/restart` exige `{"confirm":true}` e **pelo menos 75 segundos de funcionamento desde o boot**. Antes desse prazo, retorna `503` com `code=not_ready`, para preservar a janela de confirmação do boot. O limite é anunciado em `GET /capabilities`, no campo `limits.restart_min_uptime_s`.

Depois desse prazo, o pedido ainda é recusado com `409` durante água em uso ou ajuste de temperatura. Quando aceito, reinicia o **ESP32** após uma pequena espera para devolver a resposta HTTP; não reinicia nem liga/desliga a placa original do aquecedor. O reinício perde os pontos do gráfico em RAM; os usos e os acumulados são preservados.

Não existem endpoints para liga/desliga, ignição direta, alteração de proteções ou envio de comandos UART arbitrários. Consulte as capacidades anunciadas pela API antes de mostrar uma função no visor.

## Histórico

`GET /history` devolve `samples` como linhas numéricas de seis colunas, na ordem de `sample_columns`: `age_s`, `inlet_c`, `outlet_c`, `flow_l_min`, `power_kw`, `heating`. Chama é 0, 1 ou `null`. Há até 720 pontos em intervalos de 120 s, cobrindo até 24 h. Cada intervalo retém picos de vazão/potência, chama observada e últimas temperaturas válidas. Não use esses picos como se fossem uma leitura instantânea para o visor.

Os pontos ficam em RAM e começam novamente após reiniciar. `sessions` tem os 24 usos concluídos mais recentes, persistidos na flash. Registros antigos podem ter início desconhecido; `started_at_epoch_s` e `elapsed_s` ficam `null` quando não foram observados. `duration_s` é tempo acompanhado com leitura válida; pode ser diferente do tempo transcorrido entre início e fim. `complete=false` informa acompanhamento incompleto.

`GET /periods` usa datas de Brasília (UTC−3). `key` é YYYYMMDD nos dias e YYYYMM nos meses. Há até 31 dias e 12 meses. Consulte `clock_valid`, `partial` e `unassigned`; consumos herdados não foram distribuídos retroativamente por data. Esses limites são capacidades de registros, não garantias de cobertura contínua sem energia/rede.

## Sincronizar usos concluídos — firmware 1.2.0

`GET /sessions` é a coleção indicada para construir um arquivo de banhos/acionamentos em outro sistema. Diferentemente de `/history`, não transporta os 720 pontos voláteis e exporta somente usos presentes no **último checkpoint V4 gravado e sincronizado com sucesso na flash**. Um uso recém-observado que ainda não teve gravação confirmada não deve aparecer como durável.

| Campo | Semântica |
|---|---|
| `schema_version` | `1`, versão do formato desta coleção |
| `device_id` | Identidade do ESP, no formato `rinnai-` seguido por 12 dígitos hexadecimais |
| `stream_id` | 32 dígitos hexadecimais persistidos com a série histórica |
| `ready`, `storage_ok` | Verdadeiros em uma resposta 200 válida; indisponibilidade retorna 503 |
| `capacity` | `24`, capacidade do buffer de recuperação no ESP |
| `total_sessions` | Contador persistido; pode ser maior que a quantidade de registros disponíveis |
| `oldest_sequence`, `newest_sequence` | Limites disponíveis; ambos `null` se a lista estiver vazia |
| `sequence_exhausted` | Sinaliza o esgotamento do contador de sequência; não supor novos IDs |
| `pricing_basis` | `current_settings`: estimativas calculadas com as tarifas/tipo de gás atuais da resposta |
| `settings` | `gas_type`, `gas_unit`, `gas_tariff` e `water_tariff` usados na estimativa |
| `sessions` | Até 24 registros, **mais antigos primeiro**, sem o uso ainda em andamento |

Cada registro contém `id`, `sequence`, `started_at_epoch_s`, `ended_at_epoch_s`, `elapsed_s`, `duration_s`, `heating_s`, `water_l`, `energy_kwh`, `gas`, `gas_unit`, `gas_cost`, `water_cost`, `cost`, `cost_complete`, `complete` e `end_reason`. Datas e duração transcorrida desconhecidas permanecem `null`. `duration_s` continua sendo tempo acompanhado com leituras válidas.

O **`id` é `stream_id:sequence`**. Deduplicate por `device_id` e esse ID, nunca pelo horário ou pelo custo. Sincronização do relógio pode completar datas do mesmo ID, sem criar um novo banho. OTA normal/reinício preservam o stream; uma perda/reset de sua identidade ou recuperação por um arquivo antigo sem identidade cria outro stream, evitando reutilizar uma sequência como se fosse o mesmo registro.

Um consumidor confiável deve:

1. Validar o snapshot completo e importar os usos disponíveis na primeira conexão, identificando a cobertura anterior desconhecida.
2. Conciliar registros repetidos pelo ID, aceitando metadados de horário posteriores sem aumentar a contagem novamente.
3. Salvar dados e cursor localmente **antes de confirmar a importação**. Falha HTTP, 503, conteúdo inválido ou falha de disco não deve avançar o cursor.
4. Detectar lacunas entre a última sequência acompanhada e a janela disponível. Se mais de 24 usos ocorrerem durante a desconexão, parte deles pode ter sido sobrescrita no ESP; sinalize a perda, sem inventar volume/horários dos usos ausentes.
5. Tratar mudança de stream e conflitos de conteúdo explicitamente, preservando o arquivo anterior.

As tarifas de `/sessions` são atuais, como no painel do ESP. A integração **Rinnai Local** para Home Assistant conserva a estimativa e as tarifas da **primeira importação** (`tariffs_at_import`), para que uma alteração posterior de preço não reescreva silenciosamente os custos do arquivo. Isso não recupera a tarifa que era praticada antes da integração nem transforma gás estimado em medição física.

`GET /sessions` não cria eventos no Home Assistant por conta própria e não substitui a persistência do consumidor. O [guia da integração de histórico](../home_assistant/README.md) descreve instalação, recuperação, calendário e consultas paginadas.

## Exemplos

PowerShell:

```powershell
$base = 'http://192.168.1.50/api/v1'
Invoke-RestMethod "$base/telemetry"

# Envie uma vez e acompanhe o pedido aceito.
$pedido = Invoke-RestMethod -Method Post -Uri "$base/temperature" `
  -ContentType 'application/json' -Body (@{target=38} | ConvertTo-Json -Compress)
Invoke-RestMethod "$base/control"
```

curl, em Linux/macOS ou terminal com sintaxe de shell compatível:

```sh
curl --fail 'http://192.168.1.50/api/v1/telemetry'
curl --fail -X POST 'http://192.168.1.50/api/v1/temperature' \
  -H 'Content-Type: application/json' -d '{"target":38}'
curl --fail 'http://192.168.1.50/api/v1/control'
```

Leitura para um visor com Python, sem dependências adicionais:

```python
import json
import time
from urllib.request import urlopen

base = "http://192.168.1.50/api/v1"
while True:
    try:
        with urlopen(base + "/telemetry", timeout=3) as response:
            data = json.load(response)
        current = data["online"] and not data["readings"]["setpoint"]["stale"]
        value = data["setpoint_c"] if current else None
        label = f"{value} °C" if value is not None else "Sem leitura atual"
    except (OSError, ValueError, KeyError):
        label = "Sem conexão"
    print(label)  # Troque pela função de desenho do seu visor.
    time.sleep(2)
```

## Home Assistant

O ESP mantém a **API nativa ESPHome criptografada na porta 6053**. Ela expõe o número **Temperatura ajustada**, a ação **ajustar_temperatura(target)**, os sensores e o estado do ajuste. A integração ESPHome conecta diretamente ao dispositivo e entrega atualizações sem exigir que o Home Assistant fique consultando todos os endpoints HTTP. Veja a [integração ESPHome do Home Assistant](https://www.home-assistant.io/integrations/esphome/) e a [API nativa ESPHome](https://esphome.io/components/api/).

Quando for integrar, use o host `192.168.1.50`, a porta 6053 e a chave de criptografia já configurada. O nome final de cada entidade no Home Assistant depende da instalação. A API HTTP v1 é útil para um visor independente, scripts e clientes que preferem JSON; ela pode coexistir com a conexão nativa.

Para o histórico **por acionamento**, instale também a integração opcional **Rinnai Local**, que consulta `/sessions` a cada dez segundos e mantém seu próprio arquivo persistente no Home Assistant. As entidades ESPHome continuam responsáveis pela telemetria/controle; o histórico detalhado de usos é complementar e não requer MQTT. Consulte o [guia da integração](../home_assistant/README.md).

## Erros

Os erros tratados pela API v1 usam `{"error":"mensagem legível","code":"codigo_estavel"}`. Use o HTTP e `code` para lógica; a mensagem pode mudar.

| HTTP | Situação |
|---|---|
| 400 | JSON, campos ou tipos inválidos |
| 403 | Origem de navegador recusada |
| 404 | Rota desconhecida |
| 405 | Método não implementado |
| 408 | Tempo esgotado ao receber o corpo |
| 409 | Controle/fila ocupado ou reinício incompatível com o estado atual |
| 413 | Corpo acima de 2048 bytes |
| 415 | Content-Type diferente de application/json |
| 503 | Estado inicial/degradado, pré-condições de comunicação indisponíveis ou reinício solicitado antes dos 75 s de boot |

O transporte pode recusar uma requisição antes do handler da API, por exemplo se o POST não enviar Content-Length. Bibliotecas HTTP comuns e os exemplos acima calculam esse cabeçalho automaticamente. Não há CORS aberto para outros sites: clientes de rede sem Origin são aceitos, e uma página de navegador deve ter a mesma origem/host permitido.
