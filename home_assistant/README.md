# Home Assistant — controle e histórico por acionamento

As duas integrações podem funcionar juntas:

| Integração no HA | Finalidade | Comunicação |
|---|---|---|
| **ESPHome**, nativa do Home Assistant | Sensores atuais e ajuste confirmado de temperatura, de 35 a 50 °C | API ESPHome criptografada, porta 6053 |
| **Rinnai Local**, componente deste repositório | Arquivo persistente de usos concluídos, calendário e consultas paginadas | `GET /api/v1/sessions`, a cada 10 segundos |

Não é necessário MQTT. O painel/PWA hospedado no ESP continua disponível. **`192.168.1.50` é um endereço ilustrativo**; substitua pelo IP do seu ESP32.

## Requisitos

- Firmware **1.2.0 ou posterior**, anunciando `capabilities.completed_sessions=true`.
- Home Assistant **2026.10.4** é a versão de referência.
- Conectividade HTTP local entre Home Assistant e ESP.
- Uma cópia de [`custom_components/rinnai_local`](custom_components/rinnai_local) no diretório de configuração do HA.

Rinnai Local lê os usos concluídos. Ela não envia temperatura, não reinicia o ESP e não atua no aquecedor; o controle permanece no caminho nativo ESPHome.

## Instalar o histórico Rinnai Local

1. Copie a pasta inteira `home_assistant/custom_components/rinnai_local` deste repositório para **`/config/custom_components/rinnai_local`** no Home Assistant. `/config` representa o diretório de configuração da sua instalação.
2. Reinicie o Home Assistant uma vez para carregar o componente.
3. Abra **Configurações → Dispositivos e serviços → Adicionar integração** e procure **Rinnai Local**.
4. Informe a URL base do ESP, por exemplo **`http://192.168.1.50`**, sem `/api/v1`.
5. Configure o limite de registros e conclua. A identidade lida do ESP impede cadastrar acidentalmente o mesmo dispositivo duas vezes.
6. Confira as entidades e atribua o dispositivo à área desejada.

O limite padrão do arquivo HA é **10.000 registros**, configurável até **50.000**. Ao atingir o limite, a integração sinaliza erro e interrompe a importação; não apaga os usos antigos silenciosamente. Alterar esse limite não aumenta o buffer de 24 registros do ESP.

O formulário HTTP não utiliza a chave ESPHome. O endpoint local de histórico não exige autenticação HTTP; a chave nativa e a senha OTA pertencem às suas respectivas funções.

## Adicionar o controle ESPHome

1. Em **Configurações → Dispositivos e serviços → Adicionar integração**, selecione **ESPHome**.
2. Informe o IP/hostname do ESP e a porta **6053**.
3. Quando solicitado, informe `api_encryption_key` da sua instalação.
4. Confira **Temperatura ajustada**, os sensores, o estado do ajuste e a conectividade.

A temperatura aparece como entidade numérica, com passo de 1 °C e faixa de 35–50 °C. O firmware confirma o valor lendo o aquecedor; enviar o comando não é tratado como confirmação de sucesso. O nome final da entidade depende do cadastro no HA. Veja a [documentação oficial da integração ESPHome](https://www.home-assistant.io/integrations/esphome/).

## O arquivo de cada uso

O registro reúne ID persistente/sequência/série, início e fim em UTC quando conhecidos, duração transcorrida, tempo acompanhado com leituras válidas, tempo aquecendo, água e energia, gás estimado/unidade, custos de água/gás, cobertura completa/incompleta, motivo de término e as tarifas/instante da primeira importação.

O arquivo fica no armazenamento persistente **`.storage` do Home Assistant**. Não depende de uma página aberta ou da retenção de todos os estados no Recorder e sobrevive a reinícios normais do HA. Inclua a configuração do HA nos backups; não edite seus arquivos `.storage` manualmente.

O **Recorder/Histórico de sensores** continua sendo outra função: guarda a evolução dos estados recebidos pela integração ESPHome. Ele não importa automaticamente os registros individuais que estavam no ESP antes da conexão.

## Identidade, gravação e recuperação

O ESP fornece `device_id`, um `stream_id` persistido de 32 dígitos hexadecimais e IDs no formato **`stream_id:sequence`**. Horário e custo não são chaves de identificação.

1. O ESP exporta somente o último checkpoint cuja gravação/sincronização com a flash foi confirmada.
2. O HA valida o snapshot e propõe uma atualização do arquivo.
3. A atualização é salva e a persistência confirmada antes de tornar novos registros/cursor visíveis.
4. Polls repetidos conciliam o mesmo ID, sem criar outro banho ou aumentar a contagem novamente.
5. Correções do relógio podem completar os horários do mesmo registro, mantendo sua identidade.

Falha HTTP, erro de validação ou falha de armazenamento não avança o cursor. A próxima consulta tenta novamente. Quantidades incompatíveis para um ID arquivado são sinalizadas como conflito, em vez de substituir silenciosamente o histórico.

### Primeira conexão

São importados todos os usos ainda disponíveis no ESP, **até 24**. Usos anteriores que já saíram do anel não podem ser reconstruídos pelo acumulado. A cobertura anterior desconhecida é distinguida de uma lacuna ocorrida depois que a integração começou a acompanhar o dispositivo.

### Home Assistant temporariamente offline

Se seis usos ocorrerem com o HA desligado e todos permanecerem no buffer, serão importados na reconexão sem duplicar os já salvos. **A recuperação depende da quantidade de acionamentos, não de horas:** se mais de 24 usos ocorrerem durante a indisponibilidade, parte deles pode ter sido sobrescrita. A integração identifica sequências ausentes e sinaliza a lacuna; não inventa horários, duração ou litros dos usos perdidos.

OTA normal e reinício do ESP preservam a série. Apagar a NVS ou perder a identidade persistida cria outro `stream_id`; o HA conserva as séries anteriores e acompanha a nova separadamente. Uma troca de ESP com outro `device_id` precisa de cadastro próprio.

## Horários desconhecidos e calendário

Usos com início/fim confiáveis aparecem no **calendário de usos concluídos** oferecido pela integração. O calendário é somente leitura: seus eventos representam registros do aquecedor, não compromissos para criar/editar manualmente.

Um uso sem horário confiável continua arquivado e acessível na consulta de histórico, mas **não recebe um horário inventado no calendário**. Metadados de relógio posteriores podem fazê-lo aparecer com o mesmo ID. O tempo acompanhado não é usado para inventar um início de registros antigos.

Selecione a entidade fornecida na página **Calendário** do HA ou em um cartão de calendário no dashboard. O [cartão oficial](https://www.home-assistant.io/dashboards/calendar/) oferece mês, dia e lista. A [integração Calendar](https://www.home-assistant.io/integrations/calendar/) é a base usada por essas entidades, não um componente extra para instalar manualmente.

O aquecedor identifica acionamentos de água quente. A associação a uma pessoa, banho ou torneira depende do contexto da casa; não há identificação automática do usuário.

## Custos e tarifas

O ESP informa **`pricing_basis=current_settings`**: estimativas da resposta usam tipo de gás/tarifas atuais. A integração preserva as quantidades, tarifas e estimativas da **primeira importação** de cada registro, marcadas como **`tariffs_at_import`**.

Mudar uma tarifa no painel não reescreve silenciosamente o custo de um banho arquivado no HA. Metadados de horário posteriores podem ser conciliados sem alterar essa estimativa original.

Isso não recupera tarifas de datas anteriores à integração: um uso antigo importado hoje recebe a estimativa feita com a base de preço disponível na importação. Zero na tarifa significa não informada, mantendo a parcela `null`. Com uma única parcela disponível, o total continua incompleto. Gás e custos são estimativas, não medições da concessionária.

## Consulta completa

A ação **`rinnai_local.get_history`** devolve o histórico paginado, incluindo usos sem horário. Ela consulta o arquivo HA mesmo quando o ESP está temporariamente indisponível e não aciona o aquecedor.

Exemplo em um script/automação do HA:

```yaml
action: rinnai_local.get_history
data:
  offset: 0
  limit: 100
  include_undated: true
response_variable: usos
```

`entry_id` é opcional quando há somente um arquivo. `start` e `end` aceitam datas ISO com fuso, por exemplo `2026-11-01T00:00:00-03:00`; `end` é exclusivo. A resposta contém `records`, `total`, `next_offset`, contagens de cobertura e estado da sincronização. A paginação usa ordem de importação, mais recente primeiro; o calendário usa os horários dos usos. Os sensores de último uso seguem a maior sequência conhecida da série atual, inclusive após recuperar registros antigos.

Veja o [exemplo de dashboard](dashboard.example.yaml). Os IDs apresentados correspondem aos nomes padrão; substitua pelos IDs efetivamente criados no seu HA.

Em **Ferramentas do desenvolvedor → Ações**, selecione `rinnai_local.get_history` e os campos do formulário. A consulta não limpa o arquivo. O calendário desenha somente os registros datados; a ação é o caminho para a lista completa.

## Diagnóstico

| Situação | Comportamento esperado |
|---|---|
| ESP offline | Mantém o arquivo existente e tenta novamente nas próximas consultas |
| Snapshot repetido | Não duplica registros nem contagens |
| Falha de gravação no HA | Sinaliza erro e mantém o cursor anterior |
| Relógio completa datas | Atualiza metadados do mesmo ID |
| Mais de 24 usos com HA offline | Importa os disponíveis e sinaliza as sequências faltantes |
| Usos já ausentes na primeira conexão | Indica cobertura anterior desconhecida |
| NVS apagada / stream diferente | Preserva as séries anteriores e acompanha a nova identidade |
| Contador da origem esgotado | Sinaliza a condição, sem assumir novos IDs |
| Limite de registros do HA atingido | Interrompe a importação com erro, sem exclusão silenciosa |
| Uso sem data confiável | Permanece na consulta completa, sem evento em horário arbitrário |

Consulte contagens, último uso, lacunas e diagnóstico de sincronização junto do calendário. Investigue falhas/conflitos antes de limpar o armazenamento. Logs/arquivos podem revelar hábitos da casa; remova dados pessoais antes de publicar um relatório.

## Atualizar e preservar

Preserve a configuração e seus backups. Substitua somente os arquivos do componente `rinnai_local` pelo pacote revisado e reinicie o HA para carregar a atualização. A migração do firmware preserva os usos ainda disponíveis, sem recuperar os eventos já descartados.

O arquivo persistente é identificado pelo `device_id`. Descarregar ou remover a entrada da integração não apaga esse arquivo; cadastrar novamente o mesmo dispositivo pode recuperá-lo. Não copie o arquivo de outro dispositivo para substituir sua identidade.

## Testes

O núcleo pode ser verificado sem HA, rede ou aquecedor:

```sh
python -m unittest discover -s home_assistant/tests -p test_ledger.py
```

`test_ha_adapters.py` requer um ambiente Python com o Home Assistant e as dependências correspondentes. Ele cobre confirmação de gravação, falha silenciosa do Store, integridade, recuperação, duplicatas, drenagem do writer no reload e calendário disponível offline. A referência foi validada no runtime HA 2026.10.4 / Python 3.14.9, sem acionar equipamentos nesses testes.

Não distribua um token do Home Assistant com o componente. A conexão HTTP ao ESP não o exige. Senhas de Wi-Fi/OTA e a chave nativa também não pertencem ao pacote público.

## Referências do projeto

- [README principal](../README.md).
- [API local](../esphome/API_LOCAL.md).
- [Contrato OpenAPI](../esphome/api/openapi.json).
- [Código do componente](custom_components/rinnai_local).
