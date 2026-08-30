#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <wrench-dev.h>
#include <map>
#include <string>

class Controller : public wrench::ExecutionController {
public:
    Controller(const std::shared_ptr<wrench::BareMetalComputeService> &bare_metal_compute_service,
               const std::shared_ptr<wrench::SimpleStorageService> &storage_service,
               const std::string &hostname,
               const std::string &json_file_path);

    int main() override;

    std::string getWorkflowName();

    // Expõe a carga real (segundos de uso) por host, calculada durante a
    // simulação, para que o Simulator.cpp possa reportar a utilização real
    // da plataforma em vez de um valor fixo/placeholder.
    std::map<std::string, double> getHostTotalLoad() const { return host_total_load; }
    double getMakespan() const { return makespan; }

private:
    std::shared_ptr<wrench::BareMetalComputeService> bare_metal_compute_service;
    std::shared_ptr<wrench::SimpleStorageService> storage_service;
    std::string json_file_path;

    // Preenchidos ao final de main() com os dados reais da execução
    std::map<std::string, double> host_total_load;
    double makespan = 0.0;
};

#endif // CONTROLLER_H