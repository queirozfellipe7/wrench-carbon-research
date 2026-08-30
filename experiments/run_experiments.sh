#!/usr/bin/env bash
# ==============================================================================
# run_experiments.sh
#
# Roda TODA a matriz de experimentos:
#   schedulers (heft, dls, ipeft, peft, moheft)
#     x plataformas (todos os platform_*.xml em data/platforms/)
#       x workflows (todos os wf_*.json em data/workflows/**, incluindo trials/)
#
# Cada combinação usa o binário JÁ COMPILADO de cada scheduler — nada aqui
# roda cmake/make. Por padrão, só roda workflows na plataforma do MESMO
# tamanho (wf_8_cores em platform_8gpu.xml, wf_16_cores em platform_16gpu.xml
# etc.) — use --cross-platform para rodar todo workflow em toda plataforma
# (estudo de saturação à parte, não o experimento principal).
#
# Em vez de gerar um CSV/log por execução (o que vira milhares de arquivos
# numa matriz completa), tudo é consolidado em poucos arquivos por chamada,
# organizados em subpastas dentro de results/:
#
#   results/csv/raw_tasks_{label}.csv     <- todas as linhas de tarefa
#   results/csv/run_summary_{label}.csv   <- 1 linha por execução (métricas)
#   results/logs/run_logs_{label}.log     <- todos os logs, em blocos "RUN: {id}"
#
#   {label} = tamanho(s) de --tasks usados (ex: "256", "256-512"), ou "all"
#   se nenhum filtro --tasks foi passado.
#
# Uso:
#   ./experiments/run_experiments.sh
#   ./experiments/run_experiments.sh --schedulers heft,dls
#   ./experiments/run_experiments.sh --platforms platform_8gpu.xml
#   ./experiments/run_experiments.sh --tasks 256
#   ./experiments/run_experiments.sh --cross-platform
#   ./experiments/run_experiments.sh --dry-run
# ==============================================================================

set -u  # falha em variável não definida (mas NÃO usamos `set -e`: uma
        # simulação que falha não deve abortar as demais)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLATFORMS_DIR="$ROOT_DIR/data/platforms"
WORKFLOWS_DIR="$ROOT_DIR/data/workflows"
SCHEDULERS_DIR="$ROOT_DIR/schedulers"
RESULTS_DIR="$ROOT_DIR/results"
CSV_DIR="$RESULTS_DIR/csv"
LOG_DIR="$RESULTS_DIR/logs"
# RAW_TASKS_FILE, MASTER_LOG e SUMMARY_FILE são definidos MAIS ABAIXO,
# depois de sabermos se --tasks foi passado (o nome do arquivo leva o
# tamanho de task no nome — ver seção "Nome dos arquivos de saída").
# RAW_TASKS_FILE e SUMMARY_FILE vão para CSV_DIR; MASTER_LOG vai para LOG_DIR.

mkdir -p "$CSV_DIR" "$LOG_DIR"

# Nome da pasta do scheduler -> sufixo usado no CSV gerado pelo Controller.cpp
declare -A SCHED_SUFFIX=(
    [heft]="HEFT"
    [dls]="DLS"
    [ipeft]="IPEFT"
    [peft]="PEFT"
    [moheft]="MOHEFT"
)

# --------------------------- Argumentos de linha de comando -----------------
SELECTED_SCHEDULERS=""
SELECTED_PLATFORMS=""
SELECTED_TASKS=""
CROSS_PLATFORM=false
DRY_RUN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --schedulers) SELECTED_SCHEDULERS="$2"; shift 2 ;;
        --platforms)  SELECTED_PLATFORMS="$2"; shift 2 ;;
        --tasks)      SELECTED_TASKS="$2"; shift 2 ;;
        --cross-platform) CROSS_PLATFORM=true; shift ;;
        --dry-run)    DRY_RUN=true; shift ;;
        -h|--help)
            echo "Uso: $0 [--schedulers h1,h2] [--platforms p1.xml,p2.xml] [--tasks 256,512] [--cross-platform] [--dry-run]"
            echo ""
            echo "  --schedulers  Lista separada por vírgula (padrão: todos). Ex: --schedulers heft,dls"
            echo "  --platforms   Lista de arquivos em data/platforms/ (padrão: todos). Ex: --platforms platform_8gpu.xml"
            echo "  --tasks       Tamanhos de workflow a rodar AGORA (padrão: todos os já gerados)."
            echo "                Útil para rodar em sessões separadas, ex:"
            echo "                  dia 1: $0 --tasks 256"
            echo "                  dia 2: $0 --tasks 512"
            echo "                  dia 3: $0 --tasks 1024,2048"
            echo "  --cross-platform  Roda TODO workflow em TODA plataforma (ex: wf_16_cores em"
            echo "                    platform_8gpu.xml), não só a plataforma do mesmo tamanho."
            echo "                    Padrão é NÃO fazer isso — só roda wf_Ncores em platform_Ngpu"
            echo "                    (é o experimento principal; cross-platform é um estudo à"
            echo "                    parte, de saturação, ver README seção 'Decisões de metodologia')."
            echo "  --dry-run     Só mostra o que seria executado, sem rodar de fato."
            exit 0
            ;;
        *) echo "Argumento desconhecido: $1 (use --help)"; exit 1 ;;
    esac
done

if [[ -n "$SELECTED_SCHEDULERS" ]]; then
    IFS=',' read -ra SCHEDULERS <<< "$SELECTED_SCHEDULERS"
else
    SCHEDULERS=(heft dls ipeft peft moheft)
fi

if [[ -n "$SELECTED_PLATFORMS" ]]; then
    IFS=',' read -ra PLATFORM_NAMES <<< "$SELECTED_PLATFORMS"
    PLATFORMS=()
    for p in "${PLATFORM_NAMES[@]}"; do PLATFORMS+=("$PLATFORMS_DIR/$p"); done
else
    mapfile -t PLATFORMS < <(find "$PLATFORMS_DIR" -maxdepth 1 -name "platform_*.xml" | sort)
fi

# Todos os workflows (arquivos "principais" e os de trials/), ignorando os
# blocos base do gerador (polybench_base/) que não são workflows completos.
# Se --tasks foi passado, restringe a busca só às pastas {N}_tasks/ pedidas —
# isso é o que permite rodar 256 hoje, 512 amanhã, etc., sem tocar no resto.
if [[ -n "$SELECTED_TASKS" ]]; then
    IFS=',' read -ra TASK_SIZES <<< "$SELECTED_TASKS"
    WORKFLOWS=()
    for t in "${TASK_SIZES[@]}"; do
        task_dir="$WORKFLOWS_DIR/${t}_tasks"
        if [[ ! -d "$task_dir" ]]; then
            echo "!! Aviso: pasta '$task_dir' não existe — workflows de ${t} tasks ainda não foram gerados."
            echo "   Rode antes: data/workflows/generator/generate_all_workflows.sh"
            continue
        fi
        while IFS= read -r wf; do WORKFLOWS+=("$wf"); done \
            < <(find "$task_dir" -name "wf_*.json" | sort)
    done
else
    mapfile -t WORKFLOWS < <(find "$WORKFLOWS_DIR" -name "wf_*.json" -not -path "*/generator/*" | sort)
fi

# --------------------------- Nome dos arquivos de saída ----------------------
# Os 3 arquivos de saída levam no nome o(s) tamanho(s) de task rodado(s) nesta
# chamada (ex: raw_tasks_256.csv, rodando --tasks 256; ou raw_tasks_256-512.csv,
# rodando --tasks 256,512). Sem --tasks (roda tudo), o rótulo vira "all".
# Isso evita que execuções de dias diferentes se misturem no mesmo arquivo.
if [[ -n "$SELECTED_TASKS" ]]; then
    RUN_LABEL="$(echo "$SELECTED_TASKS" | tr ',' '-')"
else
    RUN_LABEL="all"
fi

RAW_TASKS_FILE="$CSV_DIR/raw_tasks_${RUN_LABEL}.csv"
MASTER_LOG="$LOG_DIR/run_logs_${RUN_LABEL}.log"
SUMMARY_FILE="$CSV_DIR/run_summary_${RUN_LABEL}.csv"

echo "=============================================================="
echo " Matriz de experimentos"
echo "=============================================================="
echo "Schedulers : ${SCHEDULERS[*]}"
echo "Plataformas: ${#PLATFORMS[@]} encontrada(s)"
echo "Tasks      : ${SELECTED_TASKS:-todas as já geradas}"
echo "Workflows  : ${#WORKFLOWS[@]} encontrado(s) (candidatos, antes do casamento com plataforma)"
if $CROSS_PLATFORM; then
    echo "Modo       : CROSS-PLATFORM (todo workflow em toda plataforma)"
else
    echo "Modo       : casado (só wf_Ncores em platform_Ngpu — padrão)"
fi

# Conta o total real (não é uma multiplicação simples quando não é cross-platform,
# já que cada plataforma só roda os workflows do seu próprio tamanho)
total_planned=0
for platform in "${PLATFORMS[@]}"; do
    gpus="$(basename "$platform" .xml | sed -E 's/platform_([0-9]+)gpu/\1/')"
    if $CROSS_PLATFORM; then
        matched=${#WORKFLOWS[@]}
    else
        matched=0
        for wf in "${WORKFLOWS[@]}"; do
            [[ "$(basename "$wf")" == *"_${gpus}_cores"* ]] && matched=$((matched + 1))
        done
    fi
    total_planned=$((total_planned + matched))
done
total_planned=$((total_planned * ${#SCHEDULERS[@]}))
echo "Total de execuções planejadas: $total_planned"
echo "=============================================================="

if [[ ! -f "$SUMMARY_FILE" ]]; then
    echo "scheduler,platform_gpus,workflow,workflow_seed,status,makespan_s,utilization_pct,energy_kwh,carbon_gco2,timestamp" > "$SUMMARY_FILE"
fi

if [[ ! -f "$RAW_TASKS_FILE" ]]; then
    echo "scheduler,platform_gpus,workflow,workflow_seed,task_id,host,finish_time,load_seconds" > "$RAW_TASKS_FILE"
fi

extract_gpu_count() {
    # platform_32gpu.xml -> 32
    basename "$1" .xml | sed -E 's/platform_([0-9]+)gpu/\1/'
}

run_count=0
fail_count=0

for scheduler in "${SCHEDULERS[@]}"; do
    BIN="$SCHEDULERS_DIR/$scheduler/build/my-wrench-simulator"
    SUFFIX="${SCHED_SUFFIX[$scheduler]:-}"

    if [[ -z "$SUFFIX" ]]; then
        echo "!! Scheduler desconhecido: $scheduler (não está em SCHED_SUFFIX). Pulando."
        continue
    fi
    if [[ ! -x "$BIN" ]]; then
        echo "!! Binário não encontrado para '$scheduler' ($BIN). Compile antes com cmake/make. Pulando."
        continue
    fi

    BUILD_DIR="$(dirname "$BIN")"

    for platform in "${PLATFORMS[@]}"; do
        gpus="$(extract_gpu_count "$platform")"

        # Por padrão, só roda workflows que "casam" com o tamanho desta
        # plataforma (wf_..._${gpus}_cores em platform_${gpus}gpu.xml).
        # Rodar wf_16_cores numa plataforma de 8 GPUs (por exemplo) é um
        # experimento à parte (estudo de saturação), não o principal — só
        # acontece se --cross-platform for passado explicitamente.
        if $CROSS_PLATFORM; then
            MATCHED_WORKFLOWS=("${WORKFLOWS[@]}")
        else
            MATCHED_WORKFLOWS=()
            for wf in "${WORKFLOWS[@]}"; do
                if [[ "$(basename "$wf")" == *"_${gpus}_cores"* ]]; then
                    MATCHED_WORKFLOWS+=("$wf")
                fi
            done
        fi

        for workflow in "${MATCHED_WORKFLOWS[@]}"; do
            workflow_stem="$(basename "$workflow" .json)"
            run_id="${scheduler}_${gpus}gpu_${workflow_stem}"
            expected_csv="$BUILD_DIR/resultados_${workflow_stem}_${SUFFIX}.csv"

            # Seed embutido no próprio JSON do workflow (campo "seed": N no
            # topo do arquivo) — extraído aqui para ficar no sumário, sem
            # precisar reabrir o workflow manualmente depois.
            workflow_seed="$(grep -oP '"seed":\s*\K[0-9]+' "$workflow" | head -1)"

            echo ">> [$scheduler | ${gpus} GPUs | $workflow_stem | seed=${workflow_seed:-N/A}]"

            if $DRY_RUN; then
                echo "   (dry-run, não executado)"
                continue
            fi

            run_count=$((run_count + 1))
            timestamp="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

            # Roda a execução para um log TEMPORÁRIO (usado só para extrair
            # as métricas abaixo). O conteúdo dele é anexado ao log mestre
            # logo em seguida, como um bloco identificado — nunca vira um
            # arquivo permanente por execução.
            tmp_log="$(mktemp)"
            ( cd "$BUILD_DIR" && "./$(basename "$BIN")" "$platform" "$workflow" ) > "$tmp_log" 2>&1
            exit_code=$?

            status="ok"
            if [[ $exit_code -ne 0 ]] || [[ ! -f "$expected_csv" ]]; then
                status="fail"
                fail_count=$((fail_count + 1))
                echo "   !! FALHOU (exit_code=$exit_code). Ver bloco '$run_id' em $MASTER_LOG"
            fi

            # Extrai métricas-resumo do log temporário (antes de descartá-lo)
            makespan="$(grep -oP 'Makespan Total:\s*\K[0-9.]+' "$tmp_log" | tail -1)"
            utilization="$(grep -oP 'Utiliza.*:\s*\K[0-9.]+' "$tmp_log" | tail -1)"
            energy_kwh="$(grep -oP 'Energia total consumida:\s*\K[0-9.]+' "$tmp_log" | tail -1)"
            carbon="$(grep -oP 'Pegada de carbono total:\s*\K[0-9.]+' "$tmp_log" | tail -1)"

            # Anexa o bloco completo (cabeçalho + saída) ao log mestre único
            {
                echo "===================================================================="
                echo "RUN: $run_id"
                echo "scheduler=$scheduler gpus=$gpus workflow=$workflow_stem seed=${workflow_seed:-N/A} status=$status timestamp=$timestamp"
                echo "===================================================================="
                cat "$tmp_log"
                echo ""
            } >> "$MASTER_LOG"
            rm -f "$tmp_log"

            # Consolida o CSV individual no CSV mestre (results/raw_tasks.csv),
            # prefixando cada linha com scheduler/plataforma/workflow/seed —
            # em vez de acumular um arquivo pequeno por execução (1395+ arquivos
            # numa matriz completa), tudo fica em UM único CSV, filtrável depois
            # por essas colunas em pandas/Excel/etc.
            if [[ -f "$expected_csv" ]]; then
                tail -n +2 "$expected_csv" | awk -F, -v OFS=, \
                    -v sch="$scheduler" -v gpu="$gpus" -v wf="$workflow_stem" -v seed="${workflow_seed:-}" \
                    '{print sch,gpu,wf,seed,$0}' >> "$RAW_TASKS_FILE"
                rm -f "$expected_csv"
            fi

            echo "${scheduler},${gpus},${workflow_stem},${workflow_seed:-},${status},${makespan:-},${utilization:-},${energy_kwh:-},${carbon:-},${timestamp}" >> "$SUMMARY_FILE"
        done
    done
done

echo "=============================================================="
echo " Concluído: $run_count execuções, $fail_count falha(s)"
echo " CSV mestre (todas as linhas): $RAW_TASKS_FILE"
echo " Log mestre (todos os blocos): $MASTER_LOG"
echo " Sumário agregado            : $SUMMARY_FILE"
echo "=============================================================="