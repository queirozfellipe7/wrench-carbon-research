/**
 * Copyright (c) 2017-2021. The WRENCH Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

/**
 ** An execution controller to execute a workflow
 **/

#include "Controller.h"
#include <fstream>
#include <limits>
#include <algorithm>
#include <functional>
#include <iomanip>
#include <cmath>
#include "json.hpp"

WRENCH_LOG_CATEGORY(controller, "Log category for Controller");

using json = nlohmann::json;

Controller::Controller(
    const std::shared_ptr<wrench::BareMetalComputeService> &bare_metal_compute_service,
    const std::shared_ptr<wrench::SimpleStorageService> &storage_service,
    const std::string &hostname,
    const std::string &json_file_path
) : wrench::ExecutionController(hostname, "controller"),
    bare_metal_compute_service(bare_metal_compute_service),
    storage_service(storage_service),
    json_file_path(json_file_path) {}

// Função para computar PCT (pessimista)
std::map<std::pair<std::string, std::string>, double> Controller::compute_pct(
    const std::map<std::string, double>& comp_cost,
    const std::map<std::string, std::vector<std::string>>& successors,
    const std::vector<std::string>& host_list,
    const std::map<std::string, double>& hosts_capabilities,
    const std::vector<std::string>& task_order_reverse,
    const std::map<std::string, double>& io_bytes,
    const std::map<std::string, double>& host_io_bandwidth,
    const std::map<std::pair<std::string, std::string>, double>& edge_data_bytes
) {
    std::map<std::pair<std::string, std::string>, double> pct;

    for (const auto& task : task_order_reverse) {
        for (const auto& host : host_list) {
            auto key = std::make_pair(task, host);

            // CORREÇÃO (fórmula PCT, Eq. 11 de Zhou et al., 2016), mesmo
            // padrão de bug já corrigido na OCT do PEFT: PCT(vi,pk) =
            // max_{vj em succ(vi)} { max_{pm} [ PCT(vj,pm) + w(vj,pm) +
            // c_i,j ] } — SEM somar o custo da PRÓPRIA tarefa vi no nível
            // externo. A versão anterior somava w_ij (custo próprio) aqui
            // fora E não somava w(vj,pm) (custo do sucessor) dentro do max
            // — os dois erros se cancelavam por indução, produzindo
            // PCT_code(t,h) = PCT_paper(t,h) + w(t,h) para todo (t,h): não
            // gerava NaN/infinito, mas divergia sistematicamente da Eq. 11
            // e contaminava o rankPCT (Eq. 13) com um viés pelo custo
            // próprio de cada tarefa.
            double max_succ = 0.0;
            if (successors.count(task)) {
                for (const auto& succ : successors.at(task)) {
                    double max_over_pm = std::numeric_limits<double>::lowest();
                    for (const auto& pm : host_list) {
                        // FRENTE A (parte 1): custo do sucessor sensível a
                        // host se for tarefa de I/O.
                        double w_succ_pm;
                        if (comp_cost.count(succ) && comp_cost.at(succ) > 0) {
                            w_succ_pm = comp_cost.at(succ) / hosts_capabilities.at(pm);
                        } else if (io_bytes.count(succ) && io_bytes.at(succ) > 0) {
                            w_succ_pm = io_bytes.at(succ) / host_io_bandwidth.at(pm);
                        } else {
                            w_succ_pm = 0.0;
                        }
                        // FRENTE A (parte 2 — a peça que faltava, mesma
                        // correção aplicada na OCT do PEFT): c_hat agora
                        // implementa "grátis se pm==host" (Eq. 1), em vez
                        // de zero incondicional — sem isso, "host" (o laço
                        // externo) nunca aparecia na conta, e PCT(t,h)
                        // saía idêntica pra qualquer h (confirmado por
                        // teste isolado no PEFT antes desta correção).
                        double bytes = edge_data_bytes.count(std::make_pair(task, succ))
                            ? edge_data_bytes.at(std::make_pair(task, succ)) : 0.0;
                        double c_hat = (pm == host) ? 0.0
                            : bytes / std::min(host_io_bandwidth.at(pm), host_io_bandwidth.at(host));
                        auto succ_key = std::make_pair(succ, pm);
                        double succ_value = pct.count(succ_key) ? pct.at(succ_key) : 0.0;
                        max_over_pm = std::max(max_over_pm, succ_value + w_succ_pm + c_hat);
                    }
                    max_succ = std::max(max_succ, max_over_pm);
                }
            }
            pct[key] = max_succ;  // SEM + w_ij — ver comentário acima
        }
    }
    return pct;
}

// Função para computar AEST e ALST
void Controller::compute_aest_alst(
    const std::map<std::string, double>& comp_cost,
    const std::map<std::string, std::vector<std::string>>& predecessors,
    const std::map<std::string, std::vector<std::string>>& successors,
    const std::vector<std::string>& host_list,
    const std::map<std::string, double>& hosts_capabilities,
    std::map<std::string, double>& aest,
    std::map<std::string, double>& alst,
    const std::vector<std::string>& task_order
) {
    // Pass forward → AEST (Eq. 6, Zhou et al. 2016): AEST(vi) = max_{vm em
    // pred(vi)} [ AEST(vm) + w(vm) + c_{m,i} ] — usa o custo do PRÓPRIO
    // PREDECESSOR (vm), não de vi. A versão anterior somava o custo do
    // PRÓPRIO vi (fora do max sobre predecessores) — uma estrutura
    // recursiva diferente que, por indução, calcula algo mais parecido com
    // "tempo de término pelo caminho mais rápido" (uma espécie de EFT
    // otimista) do que com AEST (tempo de INÍCIO mais cedo). Isso zerava a
    // detecção de nós críticos (AEST==ALST): num caso de cadeia simples
    // A->B->C testado isoladamente (sem paralelismo, logo todo nó deveria
    // ser crítico), a versão anterior não identificava NENHUM dos 3 nós
    // como crítico.
    for (const auto& task : task_order) {
        double max_pred = 0.0;
        if (predecessors.count(task)) {
            for (const auto& p : predecessors.at(task)) {
                double w_p = 0.0;  // custo do PREDECESSOR p, não de task
                if (comp_cost.count(p) && comp_cost.at(p) > 0) {
                    w_p = std::numeric_limits<double>::max();
                    for (const auto& host : host_list) {
                        if (hosts_capabilities.count(host)) {
                            w_p = std::min(w_p, comp_cost.at(p) / hosts_capabilities.at(host));
                        }
                    }
                }
                double aest_p = aest.count(p) ? aest.at(p) : 0.0;
                max_pred = std::max(max_pred, aest_p + w_p);
            }
        }
        aest[task] = max_pred;  // SEM somar o custo do próprio "task" aqui
    }

    // Pass backward → ALST (latest start time)
    double max_makespan = 0.0;
    for (const auto& task : task_order) {
        if (aest.count(task)) max_makespan = std::max(max_makespan, aest.at(task));
    }

    for (auto rit = task_order.rbegin(); rit != task_order.rend(); ++rit) {
        const auto& task = *rit;

        // CORREÇÃO (caso-base da Eq. 7, Zhou et al. 2016): ALST(vexit) =
        // AEST(vexit), atribuição DIRETA, sem subtrair peso nenhum. A
        // versão anterior tratava toda tarefa sem sucessores (não só a
        // exit "de verdade") como se caísse no fallback max_makespan e
        // ainda SUBTRAÍA min_exec dela — em vez de ALST(exit)=AEST(exit),
        // o resultado virava AEST(exit) - w(exit), quebrando o caso-base
        // da recursão inteira. Detectado testando ponta a ponta um DAG de
        // cadeia simples (A->B->C): mesmo com o AEST já corrigido, ALST
        // saía todo errado por causa deste bug independente.
        if (!successors.count(task) || successors.at(task).empty()) {
            alst[task] = aest.count(task) ? aest.at(task) : 0.0;
            continue;
        }

        double min_exec = 0.0;
        if (comp_cost.count(task) && comp_cost.at(task) > 0) {
            min_exec = std::numeric_limits<double>::max();
            for (const auto& host : host_list) {
                if (hosts_capabilities.count(host)) {
                    min_exec = std::min(min_exec, comp_cost.at(task) / hosts_capabilities.at(host));
                }
            }
        }
        double min_succ = max_makespan;
        for (const auto& s : successors.at(task)) {
            if (alst.count(s)) min_succ = std::min(min_succ, alst.at(s));
        }
        alst[task] = min_succ - min_exec;
    }
}

// Função para computar CNCT
std::map<std::pair<std::string, std::string>, double> Controller::compute_cnct(
    const std::set<std::string>& critical_nodes,
    const std::map<std::string, double>& comp_cost,
    const std::map<std::string, std::vector<std::string>>& successors,
    const std::vector<std::string>& host_list,
    const std::map<std::string, double>& hosts_capabilities,
    const std::vector<std::string>& task_order_reverse,
    const std::map<std::string, double>& io_bytes,
    const std::map<std::string, double>& host_io_bandwidth,
    const std::map<std::pair<std::string, std::string>, double>& edge_data_bytes
) {
    std::map<std::pair<std::string, std::string>, double> cnct;

    // CORREÇÃO (Eq. 12, Zhou et al. 2016) — três problemas na versão
    // anterior:
    // (a) Faltava o ramo "caso contrário" da Eq. 12: se NENHUM sucessor é
    //     nó crítico, a fórmula manda usar TODOS os sucessores, não pular
    //     a tarefa inteira. A versão anterior só considerava sucessores
    //     que já eram CN — tarefas sem nenhum sucessor crítico ficavam com
    //     CNCT=0 sempre, quando deveriam ter um valor calculado a partir
    //     de todos os seus sucessores.
    // (b) A recursão usava pct_table (tabela PCT) em vez de recursão no
    //     PRÓPRIO cnct — a Eq. 12 exige CNCT(vj,pm), não PCT(vj,pm).
    // (c) Faltava o termo w(vj,pm) (custo do sucessor no host candidato)
    //     dentro do min — mesmo padrão de bug já corrigido na PCT/OCT.
    // Também corrigida a ordem de chamada: precisa ser task_order_reverse
    // (bottom-up), não task_order (entrada->saída) — sem isso, ao
    // processar uma tarefa, os valores de CNCT dos seus sucessores ainda
    // não teriam sido calculados.
    for (const auto& task : task_order_reverse) {
        for (const auto& host : host_list) {
            auto key = std::make_pair(task, host);

            bool has_cn_succ = false;
            if (successors.count(task)) {
                for (const auto& succ : successors.at(task)) {
                    if (critical_nodes.count(succ)) { has_cn_succ = true; break; }
                }
            }

            double max_succ = 0.0;
            if (successors.count(task)) {
                for (const auto& succ : successors.at(task)) {
                    // Ramo principal: só sucessores que são CN, SE existir
                    // algum. Ramo "caso contrário": todos os sucessores,
                    // se nenhum for CN.
                    if (has_cn_succ && !critical_nodes.count(succ)) continue;

                    double min_over_pm = std::numeric_limits<double>::max();
                    for (const auto& pm : host_list) {
                        // FRENTE A (parte 1): custo do sucessor sensível a
                        // host se for tarefa de I/O.
                        double w_succ_pm;
                        if (comp_cost.count(succ) && comp_cost.at(succ) > 0) {
                            w_succ_pm = comp_cost.at(succ) / hosts_capabilities.at(pm);
                        } else if (io_bytes.count(succ) && io_bytes.at(succ) > 0) {
                            w_succ_pm = io_bytes.at(succ) / host_io_bandwidth.at(pm);
                        } else {
                            w_succ_pm = 0.0;
                        }
                        // FRENTE A (parte 2): c_hat implementa "grátis se
                        // pm==host", mesma correção da OCT/PCT — sem isso,
                        // CNCT(t,h) sai idêntica pra qualquer h.
                        double bytes = edge_data_bytes.count(std::make_pair(task, succ))
                            ? edge_data_bytes.at(std::make_pair(task, succ)) : 0.0;
                        double c_hat = (pm == host) ? 0.0
                            : bytes / std::min(host_io_bandwidth.at(pm), host_io_bandwidth.at(host));
                        auto succ_key = std::make_pair(succ, pm);
                        double cnct_succ_pm = cnct.count(succ_key) ? cnct.at(succ_key) : 0.0;
                        min_over_pm = std::min(min_over_pm, cnct_succ_pm + w_succ_pm + c_hat);
                    }
                    max_succ = std::max(max_succ, min_over_pm);
                }
            }
            cnct[key] = max_succ;
        }
    }
    return cnct;
}

int Controller::main() {
    wrench::TerminalOutput::setThisProcessLoggingColor(wrench::TerminalOutput::COLOR_GREEN);
    WRENCH_INFO("Controller starting (IPEFT Scheduler)");

    double start_time = this->getSimulation()->getCurrentSimulatedDate();

    // 1. Leitura do JSON
    std::ifstream f(json_file_path);
    if (!f.is_open()) {
        std::cerr << "\n[ERRO FATAL] Não foi possível abrir o arquivo de workflow: "
                  << json_file_path << "\n"
                  << "Verifique o caminho passado como segundo argumento do simulador.\n"
                  << std::endl;
        throw std::runtime_error("Workflow JSON não encontrado: " + json_file_path);
    }
    json data = json::parse(f);

    if (data["jobs"].empty()) {
        std::cerr << "\n[ERRO FATAL] O workflow foi lido, mas contém ZERO jobs: "
                  << json_file_path << "\n" << std::endl;
        throw std::runtime_error("Workflow sem jobs: " + json_file_path);
    }

    // 2. Criação dos arquivos
    std::map<std::string, std::shared_ptr<wrench::DataFile>> files;
    // FRENTE A: tamanho de cada arquivo em bytes, capturado à parte dos
    // objetos DataFile do WRENCH — necessário para calcular custo de
    // transferência sensível a host (io_bytes) e custo de comunicação por
    // aresta (edge_data_bytes), mesmo padrão já aplicado no PEFT.
    std::map<std::string, double> file_sizes;
    for (auto const &file : data["files"]) {
        std::string file_id = file["id"];
        sg_size_t file_size = file["size"].get<sg_size_t>();
        file_sizes[file_id] = static_cast<double>(file_size);
        files[file_id] = wrench::Simulation::addFile(file_id, file_size);
        wrench::StorageService::createFileAtLocation(
            wrench::FileLocation::LOCATION(this->storage_service, files[file_id])
        );
    }

    auto job_manager = this->createJobManager();

    // 3. Criação dos jobs e grafo
    std::map<std::string, std::shared_ptr<wrench::CompoundJob>> jobs;
    std::map<std::string, double> comp_cost;
    std::map<std::string, std::vector<std::string>> successors;
    std::map<std::string, std::vector<std::string>> predecessors;
    // FRENTE A: bytes a transferir por tarefa de I/O, e arquivos de
    // entrada/saída de cada tarefa (via "uses" para tarefas de computação,
    // file_reads/file_writes para tarefas de I/O) — usados logo abaixo
    // para calcular edge_data_bytes (dado transferido em cada aresta do
    // DAG), mesmo padrão já validado no PEFT.
    std::map<std::string, double> io_bytes;
    std::map<std::string, std::vector<std::string>> task_output_files;
    std::map<std::string, std::vector<std::string>> task_input_files;

    for (auto const &task : data["jobs"]) {
        std::string id = task["id"];
        auto job = job_manager->createCompoundJob(id);
        jobs[id] = job;

        if (task.contains("uses")) {
            for (auto const &u : task["uses"]) {
                std::string file_id = u["id"];
                std::string type = u["type"];
                if (type == "output") task_output_files[id].push_back(file_id);
                else if (type == "input") task_input_files[id].push_back(file_id);
            }
        }

        if (task.contains("file_reads") && !task["file_reads"].empty()) {
            std::string file_id = task["file_reads"][0]["id"];
            job->addFileReadAction(id, wrench::FileLocation::LOCATION(this->storage_service, files[file_id]));
            comp_cost[id] = 0.0;
            io_bytes[id] = file_sizes.count(file_id) ? file_sizes.at(file_id) : 0.0;
            task_input_files[id].push_back(file_id);
        } else if (task.contains("file_writes") && !task["file_writes"].empty()) {
            std::string file_id = task["file_writes"][0]["id"];
            job->addFileWriteAction(id, wrench::FileLocation::LOCATION(this->storage_service, files[file_id]));
            comp_cost[id] = 0.0;
            io_bytes[id] = file_sizes.count(file_id) ? file_sizes.at(file_id) : 0.0;
            task_output_files[id].push_back(file_id);
        } else {
            int cores = task.contains("cores") ? (int)task["cores"] : 1;
            double flops = task["flops"].get<double>();
            double memory = task.contains("memory") ? task["memory"].get<double>() : 0.0;
            job->addComputeAction(id, flops, memory, 1, cores, wrench::ParallelModel::AMDAHL(0.9));
            comp_cost[id] = flops;
        }
    }

    // FRENTE A: bytes transferidos em cada aresta de dependência,
    // cruzando os arquivos de SAÍDA do pai com os de ENTRADA do filho.
    std::map<std::pair<std::string, std::string>, double> edge_data_bytes;
    if (data.contains("dependencies")) {
        for (auto const &dep : data["dependencies"]) {
            for (auto const &p : dep["parents"]) {
                for (auto const &c : dep["children"]) {
                    std::string parent = p;
                    std::string child = c;
                    predecessors[child].push_back(parent);
                    successors[parent].push_back(child);
                    jobs[child]->addParentJob(jobs[parent]);

                    double bytes = 0.0;
                    if (task_output_files.count(parent) && task_input_files.count(child)) {
                        for (auto const &out_f : task_output_files.at(parent)) {
                            for (auto const &in_f : task_input_files.at(child)) {
                                if (out_f == in_f && file_sizes.count(out_f)) {
                                    bytes += file_sizes.at(out_f);
                                }
                            }
                        }
                    }
                    edge_data_bytes[std::make_pair(parent, child)] = bytes;
                }
            }
        }
    }

    // 5. Hosts disponíveis
    auto hosts_capabilities = bare_metal_compute_service->getCoreFlopRate();
    std::vector<std::string> host_list;
    for (auto const &h : hosts_capabilities) host_list.push_back(h.first);

    // FRENTE A: banda de I/O por host, lida da propriedade io_bandwidth_Bps
    // do platform.xml — mesmo padrão já validado no PEFT.
    std::map<std::string, double> host_io_bandwidth;
    for (const auto& h : host_list) {
        double bw = 1e9;  // fallback conservador: 1 GB/s
        auto sg_host = simgrid::s4u::Host::by_name_or_null(h);
        const char* bw_str = sg_host ? sg_host->get_property("io_bandwidth_Bps") : nullptr;
        if (bw_str) {
            try { bw = std::stod(bw_str); } catch (const std::exception&) {
                WRENCH_WARN("io_bandwidth_Bps inválido no host %s, usando fallback.", h.c_str());
            }
        } else {
            WRENCH_WARN("Host %s sem propriedade io_bandwidth_Bps; usando fallback.", h.c_str());
        }
        host_io_bandwidth[h] = bw;
    }

    // Helper único para tempo de execução de uma tarefa num host — usado
    // tanto em compute_pct/compute_cnct quanto na Fase de alocação, para
    // garantir que todos usem exatamente a mesma convenção de custo.
    auto get_exec_time = [&](const std::string& task_id, const std::string& hostname) -> double {
        if (comp_cost.count(task_id) && comp_cost.at(task_id) > 0) {
            return comp_cost.at(task_id) / hosts_capabilities.at(hostname);
        } else if (io_bytes.count(task_id) && io_bytes.at(task_id) > 0) {
            return io_bytes.at(task_id) / host_io_bandwidth.at(hostname);
        }
        return 0.001;
    };

    // 6. Cálculo prévio de avg_flop_rate
    double avg_flop_rate = 0.0;
    for (auto const &h : hosts_capabilities) avg_flop_rate += h.second;
    if (!hosts_capabilities.empty()) avg_flop_rate /= hosts_capabilities.size();
    else avg_flop_rate = 1.0;

    // 7. Declaração e cálculo de rank_u (agora com captura correta)
    std::map<std::string, double> rank_u;
    std::function<double(const std::string&)> compute_rank = [&](const std::string &id) -> double {
        if (rank_u.count(id)) return rank_u[id];
        double w = (comp_cost.count(id) && comp_cost.at(id) > 0) ? comp_cost.at(id) / avg_flop_rate : 0.0;
        double max_succ = 0.0;
        if (successors.count(id)) {
            for (auto const &s : successors.at(id)) {
                max_succ = std::max(max_succ, compute_rank(s));
            }
        }
        rank_u[id] = w + max_succ;
        return rank_u[id];
    };
    for (auto const &j : jobs) compute_rank(j.first);

    // 8. Declaração de task_order
    std::vector<std::string> task_order;
    for (auto const &j : jobs) task_order.push_back(j.first);

    // Ordenação inicial (topológica aproximada)
    std::sort(task_order.begin(), task_order.end(), [&](const std::string &a, const std::string &b) {
        return rank_u[a] > rank_u[b];
    });

    // 9. Computar PCT
    std::vector<std::string> task_order_reverse = task_order;
    std::reverse(task_order_reverse.begin(), task_order_reverse.end());
    auto pct_table = compute_pct(comp_cost, successors, host_list, hosts_capabilities, task_order_reverse, io_bytes, host_io_bandwidth, edge_data_bytes);

    // 10. Computar AEST/ALST e identificar CN/CNP
    std::map<std::string, double> aest, alst;
    compute_aest_alst(comp_cost, predecessors, successors, host_list, hosts_capabilities, aest, alst, task_order);

    std::set<std::string> critical_nodes;
    for (const auto& task : task_order) {
        if (alst.count(task) && aest.count(task) && alst.at(task) == aest.at(task)) {
            critical_nodes.insert(task);
        }
    }

    std::set<std::string> critical_parents;
    for (const auto& cn : critical_nodes) {
        if (predecessors.count(cn)) {
            for (const auto& p : predecessors.at(cn)) critical_parents.insert(p);
        }
    }

    // CORREÇÃO: task_order_reverse (bottom-up), não task_order — a Eq. 12
    // exige recursão no próprio CNCT, que precisa dos sucessores já
    // calculados. comp_cost/hosts_capabilities substituem pct_table (ver
    // comentário em compute_cnct).
    auto cnct_table = compute_cnct(critical_nodes, comp_cost, successors, host_list, hosts_capabilities, task_order_reverse, io_bytes, host_io_bandwidth, edge_data_bytes);

    // 11. Priorização IPEFT
    std::map<std::string, double> rank_ipeft;
    for (const auto& task : task_order) {
        double sum_pct = 0.0;
        double avg_w_i = 0.0;
        int count = 0;
        for (const auto& host : host_list) {
            auto key = std::make_pair(task, host);
            if (pct_table.count(key)) sum_pct += pct_table.at(key);
            if (comp_cost.count(task) && hosts_capabilities.count(host) && comp_cost.at(task) > 0) {
                avg_w_i += comp_cost.at(task) / hosts_capabilities.at(host);
                count++;
            }
        }
        avg_w_i = (count > 0) ? avg_w_i / count : 0.0;
        rank_ipeft[task] = (host_list.empty() ? 0.0 : sum_pct / host_list.size()) + avg_w_i;
    }

    // Reordena task_order por rank_ipeft decrescente
    std::sort(task_order.begin(), task_order.end(), [&](const std::string &a, const std::string &b) {
        return rank_ipeft[a] > rank_ipeft[b];
    });

    // 12. Alocação IPEFT
    std::map<std::string, std::string> scheduling_decision;
    std::map<std::string, double> task_finish_time;
    std::map<std::string, double> host_available_time;
    // host_total_load agora é membro da classe, preenchido com medição REAL
    // (compute + I/O) após a execução — ver seção 13.
    for (const auto& h : host_list) {
        host_available_time[h] = 0.0;
        host_total_load[h] = 0.0;
    }

    for (const auto& id : task_order) {
        double best_psi = std::numeric_limits<double>::max();
        std::string best_host = "";

        double lambda = critical_parents.count(id) ? 0.0 : 1.0;

        for (const auto& h : hosts_capabilities) {
            std::string hostname = h.first;

            // FRENTE A (parte 2): ready_time agora é calculado POR HOST
            // candidato — antes era calculado uma vez por tarefa, ignorando
            // em qual host cada predecessora rodou (como se o dado
            // chegasse instantaneamente em qualquer host). Mesma correção
            // já validada no PEFT.
            double ready_time = 0.0;
            if (predecessors.count(id)) {
                for (const auto& p : predecessors.at(id)) {
                    if (!task_finish_time.count(p)) continue;
                    double base = task_finish_time.at(p);
                    std::string p_host = scheduling_decision.count(p) ? scheduling_decision.at(p) : hostname;
                    double bytes = edge_data_bytes.count(std::make_pair(p, id))
                        ? edge_data_bytes.at(std::make_pair(p, id)) : 0.0;
                    double comm = (p_host != hostname && bytes > 0)
                        ? bytes / std::min(host_io_bandwidth.at(p_host), host_io_bandwidth.at(hostname))
                        : 0.0;
                    ready_time = std::max(ready_time, base + comm);
                }
            }

            // FRENTE A: execution_time agora usa o mesmo helper que
            // compute_pct/compute_cnct usam, garantindo consistência de
            // convenção de custo entre a previsão (PCT/CNCT) e a decisão
            // real (EFT/psi).
            double execution_time = get_exec_time(id, hostname);
            double est = std::max(host_available_time[hostname], ready_time);
            double eft = est + execution_time;

            auto key = std::make_pair(id, hostname);
            double cnct = cnct_table.count(key) ? cnct_table.at(key) : 0.0;
            double psi = eft + lambda * cnct;

            // CORREÇÃO (concentração em poucos hosts, mesmo padrão já
            // encontrado e corrigido no HEFT e no PEFT — esquecido aqui na
            // primeira rodada de patches): comparação estrita
            // "psi < best_psi" deixava empates serem resolvidos pela ordem
            // de iteração de hosts_capabilities (std::map<string,double>,
            // ordem alfabética, não numérica). Evidência empírica: as 4
            // GPUs da classe LENTA (GPU5-8) receberam 77,3% de todas as
            // tarefas físicas numa execução de teste, quando deveriam
            // competir em pé de igualdade com a classe rápida na maioria
            // dos casos. Desempate explícito: em psi igual (dentro de
            // tolerância de ponto flutuante), prefere o host com MENOR
            // carga já acumulada (host_available_time).
            bool strictly_better = psi < best_psi - 1e-9;
            bool tied = std::abs(psi - best_psi) <= 1e-9;
            bool choose_this_host = strictly_better;
            if (!strictly_better && tied) {
                choose_this_host = host_available_time[hostname] < host_available_time[best_host];
            }
            if (choose_this_host) {
                best_psi = psi;
                best_host = hostname;
            }
        }

        scheduling_decision[id] = best_host;
        task_finish_time[id] = best_psi - lambda * (cnct_table.count(std::make_pair(id, best_host)) ? cnct_table.at(std::make_pair(id, best_host)) : 0.0);
        host_available_time[best_host] = task_finish_time[id];
        // (removida a estimativa teórica de host_total_load aqui — ela
        // ignorava I/O; ver medição real na seção 13, após a execução)
    }

    // 13. Execução dos jobs
    std::set<std::string> completed;
    std::set<std::string> submitted;
    std::map<std::string, std::string> actual_execution_host = scheduling_decision;

    std::map<std::string, double> task_submit_time;
    std::map<std::string, std::vector<std::pair<double, double>>> host_intervals;
    std::map<std::string, double> task_actual_duration; // info bruta p/ CSV
    // Valor REAL de conclusão de cada tarefa, medido no evento de término —
    // separado de task_finish_time (que guarda o resultado da Fase de
    // alocação estática, usado para propagar ready_time às sucessoras e
    // para host_available_time). Sem risco de contaminar decisões: toda a
    // alocação termina antes de qualquer evento real de conclusão (mesma
    // garantia já validada no HEFT/PEFT).
    std::map<std::string, double> task_real_finish_time;

    while (completed.size() < jobs.size()) {
        for (const auto& id : task_order) {
            if (submitted.count(id)) continue;
            bool ready = true;
            if (predecessors.count(id)) {
                for (const auto& p : predecessors.at(id)) {
                    if (completed.find(p) == completed.end()) {
                        ready = false;
                        break;
                    }
                }
            }
            if (ready) {
                std::map<std::string, std::string> service_args;
                service_args[id] = scheduling_decision[id];
                task_submit_time[id] = this->getSimulation()->getCurrentSimulatedDate();
                job_manager->submitJob(jobs[id], bare_metal_compute_service, service_args);
                submitted.insert(id);
            }
        }

        if (completed.size() < jobs.size()) {
            auto event = this->waitForNextEvent();
            if (auto ev = std::dynamic_pointer_cast<wrench::CompoundJobCompletedEvent>(event)) {
                std::string finished_id = ev->job->getName();
                completed.insert(finished_id);

                double now = this->getSimulation()->getCurrentSimulatedDate();
                double submit_t = task_submit_time[finished_id];
                task_actual_duration[finished_id] = now - submit_t;
                task_real_finish_time[finished_id] = now;
                host_intervals[actual_execution_host[finished_id]].push_back({submit_t, now});

            } else if (auto ev_fail = std::dynamic_pointer_cast<wrench::CompoundJobFailedEvent>(event)) {
                WRENCH_WARN("Job %s failed!", ev_fail->job->getName().c_str());
                return 1;
            }
        }
    }

    double end_time = this->getSimulation()->getCurrentSimulatedDate();
    this->makespan = end_time - start_time;

    // Merge de intervalos por host — evita dupla contagem de tarefas
    // enfileiradas no mesmo host (mesma correção aplicada no HEFT/MOHEFT).
    for (auto &entry : host_intervals) {
        auto &intervals = entry.second;
        if (intervals.empty()) continue;
        std::sort(intervals.begin(), intervals.end());

        double merged_total = 0.0;
        double cur_start = intervals[0].first;
        double cur_end   = intervals[0].second;
        for (size_t i = 1; i < intervals.size(); ++i) {
            double s = intervals[i].first, e = intervals[i].second;
            if (s <= cur_end) {
                cur_end = std::max(cur_end, e);
            } else {
                merged_total += (cur_end - cur_start);
                cur_start = s;
                cur_end   = e;
            }
        }
        merged_total += (cur_end - cur_start);
        host_total_load[entry.first] = merged_total;
    }

    // 14. Saída de resultados
    std::string csv_filename = "resultados_" + this->getWorkflowName() + "_IPEFT.csv";
    std::ofstream csv_file(csv_filename);

    if (csv_file.is_open()) {
        csv_file << "task_id,host,finish_time,load_seconds\n";
        for (const auto& id : task_order) {
            csv_file << id << ","
                     << actual_execution_host[id] << ","
                     << std::fixed << std::setprecision(4) << task_real_finish_time[id] << ","
                     << task_actual_duration[id] << "\n";
        }
        csv_file.close();
        WRENCH_INFO("Log detalhado salvo em %s", csv_filename.c_str());
    }

    std::cout << "\n============================================================" << std::endl;
    std::cout << " RESUMO DE UTILIZAÇÃO DA PLATAFORMA (IPEFT)" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << std::left << std::setw(20) << "Host" << " | " << "Tempo Total de Uso (s)" << std::endl;
    std::cout << "------------------------------------------------------------" << std::endl;

    for (const auto& h : hosts_capabilities) {
        std::cout << std::left << std::setw(20) << h.first << " | "
                  << std::fixed << std::setprecision(4) << host_total_load[h.first] << " s" << std::endl;
    }

    std::cout << "------------------------------------------------------------" << std::endl;
    std::cout << "Makespan Total: " << std::fixed << std::setprecision(4) << (end_time - start_time) << " s" << std::endl;
    std::cout << "============================================================" << std::endl;

    return 0;
}

std::string Controller::getWorkflowName() {
    size_t last_slash = json_file_path.find_last_of("/\\");
    std::string name = (last_slash == std::string::npos) ? json_file_path : json_file_path.substr(last_slash + 1);
    size_t last_dot = name.find_last_of(".");
    if (last_dot != std::string::npos) name = name.substr(0, last_dot);
    return name;
}