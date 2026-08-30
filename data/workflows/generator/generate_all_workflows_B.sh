#!/usr/bin/env bash
# generate_all_workflows.sh  —  VARIANTE OPCAO B
# CCR fixa alta (0.3) nas 3 plataformas. So faz sentido com os XML da Opcao B
# (disco do DGX acelerado p/ ~800GBps), senao o disco unico satura.
# Rode a partir da raiz do projeto.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
GENERATOR="$SCRIPT_DIR/gerador_workflow.py"
BASE_FOLDER="$SCRIPT_DIR/polybench_base"

TASKS_LIST=(256 512 1024 2048)
PLATFORMS=(8 16 32)
N_TRIALS=30

FAT=1.2
DENSITY=0.5
REGULARITY=0.5
JUMP=2
CCR=0.3          # OPCAO B: fixa nas 3 plataformas (disco acelerado suporta)

declare -A SCENARIO_ID=( [256]=0 [512]=1 [1024]=2 [2048]=3 )

for tasks in "${TASKS_LIST[@]}"; do
    scenario_id="${SCENARIO_ID[$tasks]}"
    for platform in "${PLATFORMS[@]}"; do
        out_dir="$ROOT_DIR/data/workflows/${tasks}_tasks"

        echo ">> principal: ${tasks} tasks / ${platform} GPUs (ccr=$CCR)"
        python3 "$GENERATOR" \
            --folder "$BASE_FOLDER" --tasks "$tasks" --platform "$platform" \
            --fat "$FAT" --density "$DENSITY" --regularity "$REGULARITY" --jump "$JUMP" \
            --ccr "$CCR" --seed 42 --verbose \
            --output-dir "$out_dir"

        echo ">> $N_TRIALS trials: ${tasks} tasks / ${platform} GPUs (ccr=$CCR)"
        python3 "$GENERATOR" \
            --folder "$BASE_FOLDER" --tasks "$tasks" --platform "$platform" \
            --fat "$FAT" --density "$DENSITY" --regularity "$REGULARITY" --jump "$JUMP" \
            --ccr "$CCR" --batch --n-trials "$N_TRIALS" --scenario-id "$scenario_id" \
            --output-dir "$out_dir/trials"
    done
done
echo ">> Matriz (Opcao B, ccr=$CCR) gerada em: $ROOT_DIR/data/workflows/"
