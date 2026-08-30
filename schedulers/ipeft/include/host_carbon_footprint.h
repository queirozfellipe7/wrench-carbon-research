// include/host_carbon_footprint.h

#ifndef HOST_CARBON_FOOTPRINT_H
#define HOST_CARBON_FOOTPRINT_H

#include <wrench.h> // Inclui SimGrid e WRENCH

/**
 * @brief Inicializa o plugin de pegada de carbono.
 * Deve ser chamado após a inicialização da simulação e antes de carregar a plataforma.
 */
void sg_host_carbon_footprint_plugin_init();

/**
 * @brief Retorna a pegada de carbono total de um host (em gramas de CO2).
 * @param host O host a ser consultado.
 * @return A pegada de carbono acumulada.
 */
double sg_host_get_carbon_footprint(const_sg_host_t host);

/**
 * @brief Carrega um arquivo de traço para atualizações dinâmicas da intensidade de carbono.
 * @param trace_file O caminho para o arquivo de traço.
 */
void sg_host_carbon_footprint_load_trace_file(const char* trace_file);

#endif // HOST_CARBON_FOOTPRINT_H
