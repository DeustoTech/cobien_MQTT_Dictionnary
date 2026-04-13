/**
 * @file bridge_app.c
 * @brief Application principale du pont MQTT ↔ CAN.
 *
 * Ce fichier contient la logique principale du pont :
 * - Initialisation des modules (MQTT, CAN, table de conversion)
 * - Boucle de fonctionnement (traitement en temps réel des échanges)
 * - Libération des ressources à l’arrêt
 *
 * Le pont permet de faire communiquer deux mondes :
 * - **MQTT / JSON** : échanges haut niveau avec une interface ou un serveur ;
 * - **CAN / trames 8 octets** : communication bas niveau avec les capteurs, actionneurs ou la STM32.
 *
 * L’architecture est modulaire :
 * - `table.c`  → interprète le fichier `conversion.json`
 * - `mqtt_io.c` → gère la connexion au broker et la conversion JSON → CAN
 * - `can_io.c`  → envoie et reçoit les trames CAN
 */


#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <mosquitto.h>

#include "types.h"
#include "table.h"
#include "mqtt_io.h"
#include "can_io.h"
#include "log.h"


// --- paramètres fixes par défaut ---
static const char *IFNAME    = "can0";        // ou "vcan0" en test logiciel
static const char *MQTT_HOST = "localhost";
static const int   MQTT_PORT = 1883;

/* -------------------------------------------------------------------------- */
/*                             Variables globales                             */
/* -------------------------------------------------------------------------- */


/**
 * @brief Indicateur de fonctionnement global.
 * 
 * Passe à 0 lorsqu’un signal (CTRL+C, SIGTERM) est reçu,
 * pour déclencher l’arrêt propre du programme.
 */
static volatile int g_running = 1;

/**
 * @brief Table de correspondance (topics MQTT ↔ IDs CAN).
 */
static table_t    g_table;

/**
 * @brief Contexte MQTT (connexion, QoS, callbacks...).
 */
static mqtt_ctx_t g_mqtt;

/**
 * @brief Contexte CAN (socket, interface, buffer...).
 */
static can_ctx_t  g_can;

/**
 * @brief Gestion des signaux système (SIGINT, SIGTERM).
 * 
 * Permet d’arrêter proprement la boucle principale.
 */
static void on_sig(int s){ (void)s; g_running = 0; }


/* -------------------------------------------------------------------------- */
/*                                SETUP                                       */
/* -------------------------------------------------------------------------- */


/**
 * @brief Initialise tous les modules du pont (table, MQTT, CAN).
 *
 * Étapes principales :
 * 1. Chargement du fichier `conversion.json`
 * 2. Initialisation du client MQTT et abonnement global
 * 3. Initialisation du bus CAN
 * 4. Liaison des modules entre eux via une structure commune (userdata)
 *
 * @param cfg_path Chemin du fichier JSON de configuration.
 * @return true si tout est correctement initialisé, false sinon.
 */

bool my_setup(const char *cfg_path) {
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

     /* Réinitialisation mémoire des structures globales */
    memset(&g_table, 0, sizeof(g_table));
    memset(&g_mqtt,  0, sizeof(g_mqtt));
    memset(&g_can,   0, sizeof(g_can));

     /* Chargement du fichier de conversion */
    if (!table_load(&g_table, cfg_path)) {
        LOGE("Echec chargement table: %s", cfg_path);
        return false;
    }

    /* Initialisation du client MQTT */
    if (!mqtt_init(&g_mqtt, MQTT_HOST, MQTT_PORT, 60))
        return false;

    mqtt_set_qos(&g_mqtt, 1, 1);

    /* Liaison des modules entre eux (Lier la callback MQTT -> CAN avec userdata (table+can+mqtt) */
    struct { const table_t *t; can_ctx_t *c; mqtt_ctx_t *m; } *ub =
        malloc(sizeof(*ub));
    if (!ub) return false;

    ub->t = &g_table; ub->c = &g_can; ub->m = &g_mqtt;
    mqtt_set_user_data(&g_mqtt, ub);

    /* Abonnement à tous les topics MQTT (sans doublon local) */
    if (!mqtt_subscribe_all_nolocal(&g_mqtt)) return false;

    /* Initialisation du bus CAN */
    if (!can_init(&g_can, IFNAME)) return false;

    LOGI("Setup OK (cfg=%s, if=%s, mqtt=%s:%d)", cfg_path, IFNAME, MQTT_HOST, MQTT_PORT);
    return true;
}

/* -------------------------------------------------------------------------- */
/*                                LOOP                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Boucle principale du pont.
 *
 * Cette fonction est appelée en continu
 *
 * Tâches effectuées à chaque itération :
 * 1. Traitement des paquets MQTT disponibles
 * 2. Lecture et traitement des trames CAN reçues
 *
 * @return true si le pont doit continuer à tourner, false sinon.
 */

bool my_loop(void)
{
    if (!g_running) return false;

    if (g_mqtt.mosq)
        mosquitto_loop(g_mqtt.mosq, 20, 1);

    can_poll(&g_can, &g_table, &g_mqtt, 8);
    usleep(1000);

    return true;
}

/* -------------------------------------------------------------------------- */
/*                                SHUTDOWN                                   */
/* -------------------------------------------------------------------------- */


/**
 * @brief Ferme proprement tous les modules du pont.
 *
 * Étapes :
 * 1. Libération des structures allouées
 * 2. Arrêt du CAN et du MQTT
 * 3. Libération de la table de conversion
 */

void my_shutdown(void) {
    void *ud = g_mqtt.mosq ? mosquitto_userdata(g_mqtt.mosq) : NULL;
    if (ud) free(ud);

    can_cleanup(&g_can);
    mqtt_cleanup(&g_mqtt);
    table_free(&g_table);
    LOGI("Shutdown OK %c", 0);
}

/* -------------------------------------------------------------------------- */
/*                                 MAIN                                       */
/* -------------------------------------------------------------------------- */


/**
 * @brief Point d’entrée du programme.
 *
 * Lit le fichier de configuration (par défaut `config/conversion.json`),
 * initialise le pont, puis entre dans la boucle principale.
 *
 * L’application s’arrête proprement à la réception d’un signal
 * (CTRL+C ou SIGTERM).
 *
 * @param argc Nombre d’arguments de la ligne de commande.
 * @param argv Tableau contenant les arguments.
 * @return 0 si tout s’est bien passé, 1 en cas d’erreur d’initialisation.
 */

int main(int argc, char **argv)
{
    const char *cfg_path = (argc > 1) ? argv[1] : "config/conversion.json";

    if (!my_setup(cfg_path))
        return 1;

    while (my_loop())
        ; // boucle principale

    my_shutdown();
    return 0;
}

// End of file
