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
#include <set>
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

int Controller::main() {
    wrench::TerminalOutput::setThisProcessLoggingColor(wrench::TerminalOutput::COLOR_GREEN);
    WRENCH_INFO("Controller starting (DLS Optimized Scheduler)");

    double start_time = this->getSimulation()->getCurrentSimulatedDate();

    // ---------- 1. Leitura do JSON ----------
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

    // ---------- 2. Criação dos arquivos ----------
    std::map<std::string, std::shared_ptr<wrench::DataFile>> files;
    // FRENTE A: tamanho de cada arquivo em bytes — mesmo padrão já
    // validado no HEFT/PEFT/IPEFT.
    std::map<std::string, double> file_sizes;
    for (auto const &file : data["files"]) {
        std::string file_id = file["id"];
        sg_size_t fsize = file["size"].get<sg_size_t>();
        file_sizes[file_id] = static_cast<double>(fsize);
        files[file_id] = wrench::Simulation::addFile(file_id, fsize);
        wrench::StorageService::createFileAtLocation(wrench::FileLocation::LOCATION(this->storage_service, files[file_id]));
    }

    auto job_manager = this->createJobManager();

    // ---------- 3. Criação dos jobs e Estruturas ----------
    std::map<std::string, std::shared_ptr<wrench::CompoundJob>> jobs;
    std::map<std::string, double> comp_cost;
    std::map<std::string, std::vector<std::string>> successors, predecessors;
    // FRENTE A: bytes a transferir por tarefa de I/O, e arquivos de
    // entrada/saída por tarefa — mesmo padrão já validado no HEFT/PEFT/
    // IPEFT. Antes desta correção, toda tarefa de I/O custava um floor
    // fixo de 0.001s na decisão do DLS, igual em qualquer host.
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

        // IMPORTANTE: Garantir que o job tenha uma ação para não falhar no submit
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
        } else if (task.contains("flops")) {
            double flops = task["flops"].get<double>();
            job->addComputeAction(id, flops, 0.0, 1, 1, wrench::ParallelModel::AMDAHL(0.9));
            comp_cost[id] = flops;
        } else {
            // Caso de segurança: tarefa vazia ganha um compute mínimo
            job->addComputeAction(id, 1000.0, 0.0, 1, 1, wrench::ParallelModel::AMDAHL(0.9));
            comp_cost[id] = 1000.0;
        }
    }

    // ---------- 4. Dependências ----------
    // FRENTE A: bytes transferidos em cada aresta, cruzando saída do pai
    // com entrada do filho — mesmo padrão já validado no HEFT/PEFT/IPEFT.
    std::map<std::pair<std::string, std::string>, double> edge_data_bytes;
    if (data.contains("dependencies")) {
        for (auto const &dep : data["dependencies"]) {
            for (auto const &p : dep["parents"]) {
                for (auto const &c : dep["children"]) {
                    predecessors[c].push_back(p);
                    successors[p].push_back(c);
                    jobs[c]->addParentJob(jobs[p]);

                    std::string parent = p, child = c;
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

    // ---------- 5. DLS: Cálculo de Static Level (SL) ----------
    auto hosts_capabilities = bare_metal_compute_service->getCoreFlopRate();
    std::vector<std::string> host_list;
    for (auto const &h : hosts_capabilities) host_list.push_back(h.first);

    // FRENTE A: banda de I/O por host — mesmo padrão já validado no
    // HEFT/PEFT/IPEFT.
    std::map<std::string, double> host_io_bandwidth;
    for (const auto& h : host_list) {
        double bw = 1e9;
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

    auto get_exec_time = [&](const std::string& task_id, const std::string& hostname) -> double {
        if (comp_cost.count(task_id) && comp_cost.at(task_id) > 0) {
            return comp_cost.at(task_id) / hosts_capabilities.at(hostname);
        } else if (io_bytes.count(task_id) && io_bytes.at(task_id) > 0) {
            return io_bytes.at(task_id) / host_io_bandwidth.at(hostname);
        }
        return 0.001;
    };

    // FRENTE A: median_exec_time (E*(Ni), Eq. 2.3) agora usa get_exec_time
    // em vez do floor fixo — a mediana de custo entre hosts passa a
    // refletir o custo real de transferência para tarefas de I/O.
    std::map<std::string, double> median_exec_time;
    for (auto const &j : jobs) {
        const std::string &id = j.first;
        std::vector<double> exec_times;
        exec_times.reserve(host_list.size());
        for (auto const &h : host_list) {
            exec_times.push_back(get_exec_time(id, h));
        }
        std::sort(exec_times.begin(), exec_times.end());
        size_t n = exec_times.size();
        if (n == 0) {
            median_exec_time[id] = 0.001;
        } else if (n % 2 == 1) {
            median_exec_time[id] = exec_times[n / 2];
        } else {
            median_exec_time[id] = (exec_times[n / 2 - 1] + exec_times[n / 2]) / 2.0;
        }
    }

    std::map<std::string, double> static_level;
    std::function<double(const std::string&)> compute_sl = [&](const std::string &id) -> double {
        if (static_level.count(id)) return static_level[id];
        double w = median_exec_time[id];
        double max_succ = 0.0;
        for (auto const &s : successors[id]) max_succ = std::max(max_succ, compute_sl(s));
        static_level[id] = w + max_succ;
        return static_level[id];
    };
    for (auto const &j : jobs) compute_sl(j.first);

    // ---------- 6. Loop de Execução Dinâmica (DLS Otimizado) ----------
    std::set<std::string> completed, submitted;
    std::map<std::string, std::string> actual_host;
    std::map<std::string, double> task_finish_time, host_free_time;

    for (auto const &h : hosts_capabilities) {
        host_free_time[h.first] = 0.0;
        host_total_load[h.first] = 0.0;
    }

    std::map<std::string, double> task_submit_time;
    std::map<std::string, std::vector<std::pair<double, double>>> host_intervals;
    std::map<std::string, double> task_actual_duration;
    std::map<std::string, double> task_real_finish_time;

    while (completed.size() < jobs.size()) {
        std::vector<std::string> ready_tasks;
        for (auto const &j : jobs) {
            if (submitted.count(j.first)) continue;
            bool ready = true;
            for (auto const &p : predecessors[j.first]) {
                if (completed.find(p) == completed.end()) { ready = false; break; }
            }
            if (ready) ready_tasks.push_back(j.first);
        }

        if (!ready_tasks.empty()) {
            std::string best_task = "";
            std::string best_host = "";
            double max_dl = -std::numeric_limits<double>::max();
            double best_eft = 0.0;

            for (auto const &t : ready_tasks) {
                double e_star = median_exec_time[t];

                for (auto const &h : hosts_capabilities) {
                    std::string hostname = h.first;

                    // FRENTE A (parte 2): t_ready agora é calculado POR
                    // HOST candidato, considerando de qual host cada
                    // predecessora rodou de fato (actual_host, já que o
                    // DLS submete dinamicamente, não tem "scheduling_decision"
                    // separado como HEFT/PEFT/IPEFT) e o custo de
                    // comunicação real até este host.
                    double t_ready = 0.0;
                    for (auto const &p : predecessors[t]) {
                        double base = task_finish_time[p];
                        std::string p_host = actual_host.count(p) ? actual_host.at(p) : hostname;
                        double bytes = edge_data_bytes.count(std::make_pair(p, t))
                            ? edge_data_bytes.at(std::make_pair(p, t)) : 0.0;
                        double comm = (p_host != hostname && bytes > 0)
                            ? bytes / std::min(host_io_bandwidth.at(p_host), host_io_bandwidth.at(hostname))
                            : 0.0;
                        t_ready = std::max(t_ready, base + comm);
                    }

                    double est = std::max(host_free_time[hostname], t_ready);
                    // FRENTE A (parte 1): exec_h agora sensível a host se
                    // for tarefa de I/O, via get_exec_time.
                    double exec_h = get_exec_time(t, hostname);

                    // Fórmula DLS para Sistemas Heterogêneos
                    // Eq. 2.4): DL(Ni,Pj) = SL*(Ni) - EST(Ni,Pj) + Δ(Ni,Pj),
                    // com Δ(Ni,Pj) = E*(Ni) - E(Ni,Pj).
                    double dl = static_level[t] - est + (e_star - exec_h);

                    if (dl > max_dl) {
                        max_dl = dl; best_task = t; best_host = hostname;
                        best_eft = est + exec_h;
                    }
                }
            }

            if (best_task != "") {
                actual_host[best_task] = best_host;
                task_finish_time[best_task] = best_eft;
                host_free_time[best_host] = best_eft;

                task_submit_time[best_task] = this->getSimulation()->getCurrentSimulatedDate();

                std::map<std::string, std::string> args;
                args[best_task] = best_host;
                job_manager->submitJob(jobs[best_task], bare_metal_compute_service, args);
                submitted.insert(best_task);
                continue; 
            }
        }

        if (completed.size() < jobs.size()) {
            auto event = this->waitForNextEvent();
            if (auto ev = std::dynamic_pointer_cast<wrench::CompoundJobCompletedEvent>(event)) {
                std::string finished_id = ev->job->getName();
                completed.insert(finished_id);

                double now = this->getSimulation()->getCurrentSimulatedDate();
                task_real_finish_time[finished_id] = now;

                double submit_t = task_submit_time[finished_id];
                task_actual_duration[finished_id] = now - submit_t;
                host_intervals[actual_host[finished_id]].push_back({submit_t, now});
            }
        }
    }

    double end_time = this->getSimulation()->getCurrentSimulatedDate();
    // CAMINHO 2: makespan = maior task_finish_time (modelo de escalonamento,
    // ja inclui custo de comunicacao heterogeneo). Substitui end_time-start_time
    // (relogio fisico do SimGrid), dominado pelo disco unico.
    double makespan_analitico = 0.0;
    for (auto const &kv : task_finish_time) {
        makespan_analitico = std::max(makespan_analitico, kv.second);
    }
    this->makespan = makespan_analitico;
    (void) end_time;

    // Merge de intervalos por host — evita dupla contagem de tarefas
    // enfileiradas no mesmo host 
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

    // ---------- 8. Exportação e Resumo ----------
    std::string csv_filename = "resultados_" + this->getWorkflowName() + "_DLS.csv";
    std::ofstream csv_file(csv_filename);
    if (csv_file.is_open()) {
        csv_file << "task_id,host,finish_time,load_seconds\n";
        for (auto const &id : submitted) {
            csv_file << id << "," << actual_host[id] << "," << std::fixed << std::setprecision(4) << task_real_finish_time[id] << "," << task_actual_duration[id] << "\n";
        }
        csv_file.close();
        WRENCH_INFO("Log salvo em: %s", csv_filename.c_str());
    }

    std::cout << "\n============================================================" << std::endl;
    std::cout << "           RESUMO DE UTILIZAÇÃO DA PLATAFORMA (DLS)" << std::endl;
    std::cout << "============================================================" << std::endl;
    for (auto const &h : hosts_capabilities) {
        std::cout << std::left << std::setw(20) << h.first << " | " << std::fixed << std::setprecision(4) << host_total_load[h.first] << " s (Load)" << std::endl;
    }
    std::cout << "------------------------------------------------------------" << std::endl;
    std::cout << "Makespan Total: " << std::fixed << std::setprecision(4) << this->makespan << " s" << std::endl;
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