# wrench-carbon-research

Ambiente de simulação para a avaliação comparativa de heurísticas de escalonamento de
*workflows* científicos em plataformas heterogêneas compostas por CPU e múltiplas GPUs,
considerando tempo de execução (*makespan*), consumo de energia, pegada de carbono e
utilização dos recursos.

Cinco heurísticas de escalonamento de lista são implementadas sobre o
[WRENCH](https://wrench-project.org/) e o [SimGrid](https://simgrid.org/), cada uma
como um simulador independente, e avaliadas sob as mesmas plataformas e cargas de
trabalho:

| Diretório            | Heurística | Natureza                                                    |
|----------------------|------------|-------------------------------------------------------------|
| `schedulers/heft`    | HEFT       | Lista estática, seleção pelo menor tempo de término (EFT)   |
| `schedulers/dls`     | DLS        | Lista dinâmica, reavalia o *dynamic level* a cada submissão |
| `schedulers/peft`    | PEFT       | Lista estática com *lookahead* pela tabela OCT              |
| `schedulers/ipeft`   | IPEFT      | PEFT com tratamento diferenciado do caminho crítico         |
| `schedulers/moheft`  | MOHEFT     | Multiobjetivo (*makespan* × energia), fronteira de Pareto   |

As cargas de trabalho são *workflows* sintéticos com custos computacionais amostrados
de *kernels* da suíte [PolyBench](https://github.com/MatthiasJReisinger/PolyBenchC-4.2.1)
e topologia em níveis, no modelo do gerador clássico usado na literatura de escalonamento
(parâmetros `fat`, `density`, `regularity` e `jump`).

Este repositório contém o código da dissertação de mestrado *Análise de Desempenho e
Eficiência Energética em Heurísticas de Escalonamento para Workflows Científicos em
Ambientes Heterogêneos* (PPCA/UFPA, 2026) e do artigo *Uma Avaliação Comparativa de
Heurísticas de Escalonamento para Workflows Científicos em Plataformas Heterogêneas*,
apresentado no I Workshop de Processamento de Alto Desempenho Sustentável (WPADS 2026),
no âmbito do SSCAD 2026.

---

## Estrutura do repositório

```
wrench-carbon-research/
├── schedulers/                 # um simulador WRENCH por heurística
│   ├── heft/  dls/  peft/  ipeft/  moheft/
│   │   ├── CMakeLists.txt
│   │   ├── CMakeModules/       # FindSimGrid, FindWRENCH, FindFSMod
│   │   ├── include/            # Controller.h, host_carbon_footprint.h, json.hpp
│   │   └── src/                # Controller.cpp (lógica da heurística), Simulator.cpp
├── data/
│   ├── platforms/              # platform_8gpu.xml, platform_16gpu.xml, platform_32gpu.xml
│   └── workflows/
│       └── generator/
│           ├── gerador_workflow.py          # gerador de workflows sintéticos
│           ├── generate_all_workflows.sh    # gera a matriz completa de workflows
│           └── polybench_base/              # blocos de custo extraídos do PolyBench
└── experiments/
    └── run_experiments.sh      # executa a matriz de experimentos
```

Os *workflows* gerados (`data/workflows/{N}_tasks/`) e os resultados (`results/`) não são
versionados: são regenerados de forma determinística pelos scripts abaixo.

---

## Requisitos

Versões utilizadas no estudo:

| Componente     | Versão  |
|----------------|---------|
| SimGrid        | 3.36    |
| WRENCH         | 2.5     |
| FSMod          | —       |
| Boost          | —       |
| CMake          | 3.22.1  |
| GCC (C++17)    | 11.4.0  |
| Python         | 3.10.12 |

O gerador de *workflows* utiliza apenas a biblioteca padrão do Python.

---

## Reprodução

Os comandos abaixo partem da raiz do repositório.

### 1. Compilar os cinco simuladores

```bash
for s in heft dls peft ipeft moheft; do
    (cd schedulers/$s && mkdir -p build && cd build && cmake .. && make)
done
```

Cada simulador gera o binário `schedulers/<heurística>/build/my-wrench-simulator`, que é
o caminho esperado pelo script de experimentos. Se o SimGrid, o WRENCH ou o FSMod estiverem
instalados em locais não padronizados, informe-os ao CMake:

```bash
cmake -DSimGrid_PATH=/caminho/simgrid -DWRENCH_PATH=/caminho/wrench -DFSMOD_PATH=/caminho/fsmod ..
```

### 2. Gerar as cargas de trabalho

```bash
./data/workflows/generator/generate_all_workflows.sh
```

O script gera, para cada combinação de tamanho (256, 512, 1024 e 2048 tarefas) e
plataforma (8, 16 e 32 GPUs), um *workflow* principal com semente fixa (42) e 30 repetições
com sementes determinísticas:

```
seed = (índice_do_trial × 1000) + scenario_id     # scenario_id: 0=256, 1=512, 2=1024, 3=2048
```

Os parâmetros de topologia são fixos (`fat=1.2`, `density=0.5`, `regularity=0.5`,
`jump=2`). A razão comunicação/computação não é passada explicitamente: o gerador adota
`ccr = 0,25/plataforma`, o que mantém o armazenamento único fora do caminho crítico.

Saída: `data/workflows/{N}_tasks/wf_{N}_tasks_{P}_cores.json` e
`data/workflows/{N}_tasks/trials/wf_{N}_tasks_{P}_cores_trialXX.json`.

O gerador também pode ser usado isoladamente (`python3 gerador_workflow.py --help`).

### 3. Executar os experimentos

```bash
./experiments/run_experiments.sh                         # matriz completa
./experiments/run_experiments.sh --tasks 256             # apenas um tamanho de carga
./experiments/run_experiments.sh --schedulers heft,dls   # apenas algumas heurísticas
./experiments/run_experiments.sh --dry-run               # lista as execuções sem rodá-las
```

| Opção              | Efeito                                                                      |
|--------------------|-----------------------------------------------------------------------------|
| `--schedulers`     | Heurísticas a executar (padrão: todas)                                      |
| `--platforms`      | Arquivos de plataforma a usar (padrão: todos em `data/platforms/`)          |
| `--tasks`          | Tamanhos de carga a executar; permite rodar a matriz em sessões separadas   |
| `--cross-platform` | Executa todo *workflow* em toda plataforma (ver *Decisões de metodologia*)  |
| `--dry-run`        | Mostra o que seria executado                                                |

Uma simulação isolada pode ser executada diretamente a partir do diretório `build/`:

```bash
cd schedulers/heft/build
./my-wrench-simulator ../../../data/platforms/platform_8gpu.xml \
                      ../../../data/workflows/256_tasks/wf_256_tasks_8_cores.json
```

---

## Saídas

O script consolida todas as execuções de uma chamada em três arquivos, identificados por
um rótulo com os tamanhos executados (por exemplo, `256`, `256-512` ou `all`):

| Arquivo                                 | Conteúdo                                                                                                        |
|-----------------------------------------|-----------------------------------------------------------------------------------------------------------------|
| `results/csv/run_summary_{rótulo}.csv`  | Uma linha por execução: `scheduler`, `platform_gpus`, `workflow`, `workflow_seed`, `status`, `makespan_s`, `utilization_pct`, `energy_kwh`, `carbon_gco2`, `timestamp` |
| `results/csv/raw_tasks_{rótulo}.csv`    | Uma linha por tarefa: `scheduler`, `platform_gpus`, `workflow`, `workflow_seed`, `task_id`, `host`, `finish_time`, `load_seconds` |
| `results/logs/run_logs_{rótulo}.log`    | Saída completa de cada execução, em blocos identificados por `RUN: <id>`                                         |

Uma execução é marcada como `fail` quando o binário retorna código não nulo ou não produz
o CSV esperado; falhas não interrompem a matriz. Antes de comparar heurísticas, verifique
se todas as combinações esperadas estão presentes com `status=ok`.

---

## Plataformas

Cada plataforma modela um servidor no estilo DGX: um *host* `DGX_System` (128 núcleos),
que orquestra a execução e concentra o armazenamento NVMe, e um conjunto de GPUs
heterogêneas conectadas a ele por um *link* interno. As potências (`wattage_per_state`,
no formato ociosa:máxima) e a intensidade de carbono (`carbon_intensity`) são lidas
diretamente dos arquivos XML, de modo que alterar a plataforma não exige recompilação.

Composição dos arquivos atuais:

| Host         | Velocidade | Potência ociosa:máx. (W) | 8 GPUs | 16 GPUs | 32 GPUs |
|--------------|------------|--------------------------|:------:|:-------:|:-------:|
| `DGX_System` | 34000 Gf   | 1100:3300                | 1      | 1       | 1       |
| GPU A100     | 78000 Gf   | 100:400                  | 2      | 4       | 8       |
| GPU V100     | 62000 Gf   | 90:300                   | 2      | 4       | 8       |
| GPU L4       | 40000 Gf   | 30:75                    | 2      | 4       | 8       |
| GPU T4       | 22000 Gf   | 25:70                    | 2      | 4       | 8       |

Intensidade de carbono: 473 gCO₂/kWh para todos os *hosts*.

---

## Métricas

- **Makespan:** tempo, em segundos, até a conclusão da última tarefa.
- **Tempo ocupado por host (`B_h`):** duração da união dos intervalos
  [submissão, conclusão] das tarefas executadas no *host*, após a fusão de intervalos
  sobrepostos, o que evita dupla contagem.
- **Utilização média:** média, sobre os *hosts*, de `B_h / makespan`. Como os intervalos
  começam na submissão, a métrica pode incluir espera em fila e deve ser lida como medida
  relativa entre heurísticas.
- **Energia:** calculada por *host* a partir das potências declaradas no XML, considerando
  a potência ociosa durante todo o *makespan* e o acréscimo de carga durante o tempo
  ocupado:
  `E_h = P_ociosa · makespan + (P_máx − P_ociosa) · B_h`.
  A energia total é a soma sobre todos os *hosts* físicos, inclusive os que permanecem
  ociosos.
- **Carbono:** `C_h = (E_h / 3,6×10⁶) · I_h`, com `E_h` em joules e `I_h` em gCO₂/kWh.
  Com intensidade uniforme, o carbono é proporcional à energia.

---

## Decisões de metodologia

- **Regime casado e regime cruzado.** Por padrão, cada *workflow* é executado apenas na
  plataforma para a qual foi gerado (`wf_..._8_cores` em `platform_8gpu.xml`, e assim por
  diante), o que corresponde a um estudo de escalabilidade, em que carga e plataforma
  crescem juntas. A opção `--cross-platform` executa todo *workflow* em toda plataforma,
  o que corresponde a um estudo de saturação. Resultados dos dois regimes não devem ser
  agregados.
- **Parâmetro K do MOHEFT.** O tamanho da fronteira de Pareto é definido pela constante
  `MOHEFT_K` em `schedulers/moheft/src/Controller.cpp` e condiciona quantos *hosts* o
  algoritmo efetivamente utiliza.
- **Tarefas de E/S no MOHEFT.** Como no algoritmo original, as tarefas de E/S não entram
  na busca multiobjetivo e são atribuídas ao *host* de armazenamento (`DGX_System`).
  Métricas de distribuição que contabilizem tarefas de E/S devem tratar o MOHEFT à parte.
- **Coluna `load_seconds`.** Registra o intervalo entre submissão e conclusão de cada
  tarefa e pode incluir espera em fila. Não deve ser somada por *host* como tempo de
  ocupação; serve apenas como indicador relativo de desequilíbrio de carga.

---

## Como citar

```bibtex
@mastersthesis{queiroz2026dissertacao,
  author  = {Queiroz, Fellipe Augusto Santana},
  title   = {Análise de Desempenho e Eficiência Energética em Heurísticas de Escalonamento
             para Workflows Científicos em Ambientes Heterogêneos},
  school  = {Universidade Federal do Pará, Programa de Pós-Graduação em Computação Aplicada},
  address = {Tucuruí, PA},
  year    = {2026}
}
```

Artigo: *Uma Avaliação Comparativa de Heurísticas de Escalonamento para Workflows
Científicos em Plataformas Heterogêneas*. I Workshop de Processamento de Alto Desempenho
Sustentável (WPADS 2026), SSCAD 2026.

---

## Autor

Fellipe Augusto Santana Queiroz — Programa de Pós-Graduação em Computação Aplicada,
Universidade Federal do Pará.
Orientador: Prof. Marcos Amaris Gonzalez.
