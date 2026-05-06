/**
 * @file mqtt_io.c
 * @brief Gestion des échanges MQTT pour le pont MQTT ↔ CAN.
 *
 * Ce module fait le lien entre le monde logiciel (MQTT, JSON)
 * et le monde matériel (bus CAN).  
 *
 * Il utilise la bibliothèque **libmosquitto** pour :
 * - se connecter au broker MQTT ;
 * - s’abonner à tous les topics du système ;
 * - publier les messages convertis depuis le bus CAN ;
 * - recevoir les commandes depuis MQTT et les transmettre vers le bus CAN.
 *
 * Il gère également le mode **tunnel**, utilisé par la STM32 :
 * les 2 premiers octets de la trame CAN indiquent l’ID réel du message.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>


#include <mosquitto.h>
#include <cjson/cJSON.h>

#include "types.h"
#include "table.h"
#include "pack.h"
#include "log.h"
#include "mqtt_io.h"
#include "can_io.h"


/* -------------------------------------------------------------------------- */
/*                            Configuration du pont                           */
/* -------------------------------------------------------------------------- */

/**
 * @def BRIDGE_TUNNEL_CANID
 * @brief Identifiant CAN utilisé comme canal de transport pour le mode “tunnel”.
 *
 * Dans ce mode, la STM32 envoie toutes ses trames sur cet ID fixe 
 * et le pont extrait les 2 premiers octets pour identifier le message interne réel.
 */

#ifndef BRIDGE_TUNNEL_CANID
#define BRIDGE_TUNNEL_CANID 0x431
#endif

/**
 * @brief Structure interne contenant les pointeurs nécessaires aux callbacks MQTT.
 *
 * Cette structure regroupe :
 * - la table de correspondance (topics ↔ IDs CAN)
 * - le contexte CAN
 * - le contexte MQTT
 */

typedef struct user_bundle_s
{
  const table_t *table;
  struct can_ctx_s *can;
  mqtt_ctx_t *mqtt;
} user_bundle_t;

/* -------------------------------------------------------------------------- */
/*                              Fonctions utilitaires                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Vérifie si une chaîne se termine par un suffixe donné.
 *
 * @param s Chaîne complète.
 * @param suf Suffixe à vérifier (ex: "/cmd", "/state").
 * @return 1 si le suffixe est trouvé, 0 sinon.
 */

static int
ends_with (const char *s, const char *suf)
{
  size_t a = strlen (s), b = strlen (suf);
  return (a >= b) && (strcmp (s + (a - b), suf) == 0);
}

/**
 * @brief Extrait la “base” d’un topic MQTT en supprimant les suffixes inutiles.
 *
 * Exemple :
 * - `"led/cmd"` devient `"led"`
 * - `"sensor/state"` est ignoré (non traité)
 *
 * @param in Topic MQTT complet.
 * @param[out] base Buffer où sera copiée la base du topic.
 * @param n Taille du buffer.
 */

static void
topic_base_from_input (const char *in, char *base, size_t n)
{
  if (!in || !base || !n)
    return;
  if (ends_with (in, "/state"))
    {
      base[0] = '\0';
      return;
    }                           /* ignoré */
  if (ends_with (in, "/cmd"))
    {
      size_t L = strlen (in);
      size_t nb = (L >= 4) ? (L - 4) : 0;
      if (nb >= n)
        nb = n - 1;
      memcpy (base, in, nb);
      base[nb] = '\0';
      return;
    }
  snprintf (base, n, "%s", in);
}

/* -------------------------------------------------------------------------- */
/*                              Callbacks MQTT                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief Callback exécuté lors de la connexion au broker MQTT.
 *
 * @param m Pointeur vers le client mosquitto.
 * @param ud Données utilisateur.
 * @param rc Code de retour (0 si succès).
 */

static void
on_connect (struct mosquitto *m, void *ud, int rc)
{
  (void) m;
  (void) ud;
  if (rc == 0) {
    /* marquer état connecté si possible */
    if (ud) {
      user_bundle_t *ub = (user_bundle_t *) ud;
      if (ub->mqtt)
        ub->mqtt->connected = 1;
    }
    LOGI ("MQTT connected %c", 0);
  } else {
    LOGW ("MQTT connect rc=%d", rc);
  }
}

/**
 * @brief Callback exécuté lors d’une déconnexion du broker MQTT.
 *
 * @param m Pointeur vers le client mosquitto.
 * @param ud Données utilisateur.
 * @param rc Code de retour.
 */

static void
on_disconnect (struct mosquitto *m, void *ud, int rc)
{
  (void) m;
  if (ud) {
    user_bundle_t *ub = (user_bundle_t *) ud;
    if (ub->mqtt)
      ub->mqtt->connected = 0;
  }
  LOGW ("MQTT disconnected rc=%d", rc);
}

/**
 * @brief Callback principal pour le traitement des messages MQTT entrants.
 *
 * Chaque fois qu’un message arrive sur un topic :
 * 1. On vérifie s’il correspond à une entrée de la table.
 * 2. On convertit le JSON reçu en trame binaire CAN (pack_payload()).
 * 3. On envoie la trame via le mode tunnel (ID transport fixe 0x431).
 *
 * @param m Contexte Mosquitto.
 * @param ud Données utilisateur (structure user_bundle_t).
 * @param msg Message MQTT reçu.
 */

static void
on_message (struct mosquitto *m, void *ud, const struct mosquitto_message *msg)
{
  (void) m;
  if (!ud || !msg || !msg->topic)
    return;

  user_bundle_t *ub = (user_bundle_t *) ud;
  if (!ub->table || !ub->can)
    return;

  char base[256];
  topic_base_from_input (msg->topic, base, sizeof (base));
  if (base[0] == '\0')
    return;                     /* /state ignoré */

  const entry_t *e = table_find_by_topic (ub->table, base);
  if (!e)
    {
      LOGW ("Topic inconnu: %s", msg->topic);
      return;
    }

  /* Lecture du JSON reçu */
  cJSON *in = NULL;
  if (msg->payload && msg->payloadlen > 0)
    {
      char *buf = (char *) malloc ((size_t) msg->payloadlen + 1);
      if (!buf)
        return;
      memcpy (buf, msg->payload, (size_t) msg->payloadlen);
      buf[msg->payloadlen] = '\0';
      in = cJSON_Parse (buf);
      free (buf);
    }
  if (!in)
    {
      LOGW ("Invalid JSON payload on %s", msg->topic);
      return;
    }

  /* Conversion JSON → binaire */
  uint8_t body[8] = { 0 };
  bool ok_body = pack_payload (body, e, in);
  cJSON_Delete (in);
  if (!ok_body)
    {
      LOGE ("Packing failed for topic %s", base);
      return;
    }

  /* Construction du message tunnel : [ID haut, ID bas, data...] */
  uint8_t out8[8] = { 0 };
  out8[0] = (uint8_t) ((e->can_id >> 8) & 0xFF);
  out8[1] = (uint8_t) (e->can_id & 0xFF);
  memcpy (out8 + 2, body, 6);   /* on place au plus 6 octets derrière */

  /* Envoi sur le bus CAN */
  if (!can_send (ub->can, BRIDGE_TUNNEL_CANID, out8))
    {
      LOGE ("CAN send failed (transport=0x%X, inner_id=0x%X)", BRIDGE_TUNNEL_CANID, e->can_id);
      return;
    }
  LOGI ("MQTT->CAN OK topic=%s transport=0x%X inner_id=0x%X", base, BRIDGE_TUNNEL_CANID, e->can_id);
}

/* -------------------------------------------------------------------------- */
/*                              API publique                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialise la connexion MQTT et configure les callbacks.
 *
 * @param ctx Structure du contexte MQTT à remplir.
 * @param host Adresse du broker (par défaut : localhost).
 * @param port Port du broker (par défaut : 1883).
 * @param keepalive Délai keepalive MQTT en secondes.
 * @return true si la connexion est réussie, false sinon.
 */

bool
mqtt_init (mqtt_ctx_t *ctx, const char *host, int port, int keepalive)
{
  if (!ctx)
    return false;
  memset (ctx, 0, sizeof (*ctx));
  ctx->qos_pub = 1;
  ctx->qos_sub = 1;
  ctx->connected = 0;

  mosquitto_lib_init ();
  ctx->mosq = mosquitto_new (NULL, true, NULL);
  if (!ctx->mosq)
    {
      LOGE ("mosquitto_new %c", 0);
      return false;
    }

  /* Utilisation de  MQTT v5 pour utiliser l’option “no_local” (anti-doublon) */
  mosquitto_int_option (ctx->mosq, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);

  mosquitto_connect_callback_set (ctx->mosq, on_connect);
  mosquitto_disconnect_callback_set (ctx->mosq, on_disconnect);
  mosquitto_message_callback_set (ctx->mosq, on_message);

  if (mosquitto_connect (ctx->mosq, host ? host : "localhost", port > 0 ? port : 1883, keepalive > 0 ? keepalive : 60)
      != MOSQ_ERR_SUCCESS)
    {
      LOGE ("mosquitto_connect %c", 0);
      mosquitto_destroy (ctx->mosq);
      ctx->mosq = NULL;
      mosquitto_lib_cleanup ();
      return false;
    }
  return true;
}

/**
 * @brief S’abonne à tous les topics MQTT avec l’option “NO_LOCAL”.
 *
 * Cela empêche le pont de recevoir ses propres publications (évite les boucles infinies).
 *
 * @param ctx Contexte MQTT.
 * @return true si succès, false sinon.
 */

bool
mqtt_subscribe_all_nolocal (mqtt_ctx_t *ctx)
{
  if (!ctx || !ctx->mosq)
    return false;
  /* options = bitmask, NO_LOCAL = 4 (MOSQ_SUB_OPT_NO_LOCAL) */
  int options = 4;
  int rc = mosquitto_subscribe_v5 (ctx->mosq, NULL, "#", ctx->qos_sub, options, NULL);
  if (rc != MOSQ_ERR_SUCCESS)
    {
      LOGE ("Subscribe v5 '#' rc=%d", rc);
      return false;
    }
  return true;
}

/**
 * @brief Raccourci vers mqtt_subscribe_all_nolocal().
 */

bool
mqtt_subscribe_all (mqtt_ctx_t *ctx)
{
  return mqtt_subscribe_all_nolocal (ctx);
}

/**
 * @brief Appelle périodiquement la boucle interne Mosquitto (non bloquante).
 *
 * À exécuter à chaque tour dans la boucle principale du pont.
 *
 * @param ctx Contexte MQTT.
 * @return true si la boucle s’exécute correctement.
 */

bool
mqtt_poll (mqtt_ctx_t *ctx)
{
  if (!ctx || !ctx->mosq)
    return false;
  int rc = mosquitto_loop (ctx->mosq, 0 /*timeout_ms */ , 1 /*max_packets */ );
  if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN)
    {
      LOGW ("mosquitto_loop rc=%d", rc);
      return false;
    }
  return true;
}

/**
 * @brief Publie une chaîne JSON sur un topic MQTT.
 *
 * @param ctx Contexte MQTT.
 * @param topic Nom du topic cible.
 * @param json_str Chaîne JSON à publier.
 * @return true si succès, false sinon.
 */
bool
mqtt_publish_json (mqtt_ctx_t *ctx, const char *topic, const char *json_str)
{
  if (!ctx || !ctx->mosq || !topic || !json_str)
    return false;
  int rc = mosquitto_publish (ctx->mosq, NULL, topic, (int) strlen (json_str), json_str, ctx->qos_pub, false);
  if (rc != MOSQ_ERR_SUCCESS)
    {
      LOGE ("publish '%s' rc=%d", topic, rc);
      return false;
    }
  return true;
}

/**
 * @brief Modifie les niveaux de QoS (Quality of Service) MQTT.
 *
 * @param ctx Contexte MQTT.
 * @param qos_sub QoS de souscription (0,1,2).
 * @param qos_pub QoS de publication (0,1,2).
 */
void
mqtt_set_qos (mqtt_ctx_t *ctx, int qos_sub, int qos_pub)
{
  if (!ctx)
    return;
  if (qos_sub >= 0 && qos_sub <= 2)
    ctx->qos_sub = qos_sub;
  if (qos_pub >= 0 && qos_pub <= 2)
    ctx->qos_pub = qos_pub;
}

/**
 * @brief Traite un message CAN et le publie sur MQTT.
 *
 * Cette fonction est appelée à chaque réception d’une trame CAN.
 * Elle reconvertit la trame binaire en JSON via `unpack_payload()`
 * et la publie sur le topic correspondant.
 *
 * @param ctx Contexte MQTT.
 * @param e Entrée de la table correspondant à l’ID CAN.
 * @param data Tableau de 8 octets CAN.
 * @return true si la publication réussit, false sinon.
 */
bool
mqtt_handle_can_message (mqtt_ctx_t *ctx, const entry_t *e, const uint8_t data[8])
{
  if (!ctx || !e)
    return false;
  cJSON *obj = unpack_payload (data, e);
  if (!obj)
    {
      LOGE ("Unpack failed id=0x%X", e->can_id);
      return false;
    }
  char *out = cJSON_PrintUnformatted (obj);
  cJSON_Delete (obj);
  if (!out)
    {
      LOGE ("cJSON_PrintUnformatted %c", 0);
      return false;
    }

  bool ok = mqtt_publish_json (ctx, e->topic, out);     /* publier sur le topic de base */
  free (out);
  if (ok)
    LOGI ("CAN->MQTT OK id=0x%X topic=%s", e->can_id, e->topic);
  else
    LOGE ("CAN->MQTT publish failed topic=%s", e->topic);
  return ok;
}

/**
 * @brief Enregistre un pointeur utilisateur (utile pour les callbacks MQTT).
 *
 * @param ctx Contexte MQTT.
 * @param userdata Pointeur à transmettre aux callbacks.
 */
void
mqtt_set_user_data (mqtt_ctx_t *ctx, void *userdata)
{
  if (!ctx || !ctx->mosq)
    return;
  mosquitto_user_data_set (ctx->mosq, userdata);
}

/**
 * @brief Ferme proprement la connexion MQTT.
 *
 * Déconnecte du broker, libère les ressources Mosquitto,
 * et nettoie la mémoire associée.
 *
 * @param ctx Contexte MQTT à fermer.
 */
void
mqtt_cleanup (mqtt_ctx_t *ctx)
{
  if (!ctx)
    return;
  if (ctx->mosq)
    {
      mosquitto_disconnect (ctx->mosq);
      mosquitto_destroy (ctx->mosq);
      ctx->mosq = NULL;
    }
  mosquitto_lib_cleanup ();
}

// End of file
