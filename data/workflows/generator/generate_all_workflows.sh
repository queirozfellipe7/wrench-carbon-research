#!/usr/bin/env bash
# generate_all_workflows.sh
# Gera a matriz completa de workflows: para cada combinação de
# (tamanho de tasks) x (número de GPUs), gera o workflow "principal"
# + N trials, usando o gerador_workflow.py atualizado.
#
# Uso: ./generate_all_workflows.sh
# Deve ser rodado a partir da raiz do projeto (wrench-carbon-research/)

set -e

# O script mora em: <RAIZ_DO_PROJETO>/data/workflows/generator/generate_all_workflows.sh
# Ou seja, está 3 níveis ABAIXO da raiz (generator -> workflows -> data -> raiz),
# não 1 nível acima. O cálculo anterior gerava um caminho duplicado
# (.../data/workflows/data/workflows/generator/...).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
GENERATOR="$SCRIPT_DIR/gerador_workflow.py"
BASE_FOLDER="$SCRIPT_DIR/polybench_base"

TASKS_LIST=(256 512 1024 2048)
PLATFORMS=(8 16 32)
N_TRIALS=30

# Parâmetros de topologia do gerador em níveis (fat/density/regularity/jump).
# Valores médios da faixa usada nos papers (fat=[0.1,0.4,0.8], density/
# regularity=[0.2,0.8], jump=[1,2,4]) — ajuste aqui se quiser rodar uma
# malha de sensibilidade em vez de um único ponto central.
FAT=1.2
DENSITY=0.5
REGULARITY=0.5
JUMP=2
# --ccr é OMITIDO de propósito: sem ele, o gerador usa 0.25/platform
# automaticamente, mantendo o disco único (nvme_storage) fora do caminho
# crítico. Veja CONTEXTO_PROJETO_WRENCH.md, seção sobre saturação de disco.

# scenario_id por tamanho de task, conforme fórmula de seed do paper
declare -A SCENARIO_ID=( [256]=0 [512]=1 [1024]=2 [2048]=3 )

for tasks in "${TASKS_LIST[@]}"; do
    scenario_id="${SCENARIO_ID[$tasks]}"

    for platform in "${PLATFORMS[@]}"; do
        out_dir="$ROOT_DIR/data/workflows/${tasks}_tasks"

        echo ">> Gerando workflow principal: ${tasks} tasks / ${platform} GPUs"
        python3 "$GENERATOR" \
            --folder "$BASE_FOLDER" \
            --tasks "$tasks" \
            --platform "$platform" \
            --fat "$FAT" --density "$DENSITY" --regularity "$REGULARITY" --jump "$JUMP" \
            --seed 42 \
            --output-dir "$out_dir"

        echo ">> Gerando $N_TRIALS trials: ${tasks} tasks / ${platform} GPUs"
        python3 "$GENERATOR" \
            --folder "$BASE_FOLDER" \
            --tasks "$tasks" \
            --platform "$platform" \
            --fat "$FAT" --density "$DENSITY" --regularity "$REGULARITY" --jump "$JUMP" \
            --batch --n-trials "$N_TRIALS" --scenario-id "$scenario_id" \
            --output-dir "$out_dir/trials"
    done
done

echo ">> Matriz completa gerada em: $ROOT_DIR/data/workflows/"