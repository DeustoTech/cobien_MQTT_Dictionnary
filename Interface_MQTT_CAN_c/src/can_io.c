/**
 * @file can_io.c
 * @brief Gestion des communications sur le bus CAN.
 *
 * Ce module gère la communication bas niveau avec le bus CAN :
 * - Initialisation et configuration de l’interface (socket CAN)
 * - Envoi et réception de trames 8 octets
 * - Conversion automatique entre ID CAN et topics MQTT (via table)
 *
 * Il permet donc au pont MQTT/CAN de dialoguer avec le matériel (STM32, capteurs, etc.)
 * à travers une interface comme `can0` ou `vcan0`.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/if.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include "types.h"
#include "table.h"       /**< Pour rechercher les correspondances ID ↔ topic */
#include "mqtt_io.h"     /**< Pour renvoyer les messages vers MQTT */
#include "log.h"
#include "can_io.h"


/**
 * @brief Configure un descripteur de fichier en mode non bloquant.
 * 
 * Cela permet à la lecture/écriture CAN de ne pas bloquer le programme principal.
 *
 * @param fd : descripteur de fichier (socket CAN).
 * @return true si la configuration réussit, false sinon.
 */
static bool
set_nonblock (int fd)
{
  int fl = fcntl (fd, F_GETFL, 0);
  if (fl < 0)
    return false;
  return (fcntl (fd, F_SETFL, fl | O_NONBLOCK) == 0);
}


/**
 * @brief Initialise la connexion au bus CAN (via socket PF_CAN).
 *
 * Cette fonction :
 e (RAW)
 * - associe l’interface réseau (ex : "can0", "vcan0")
 * - désactive la réception de ses propres trames (évite les doublons)
 * - configure la socket en mode non bloquant
 *
 * @param c : structure du contexte CAN à initialiser.
 * @param ifname : nom de l’interface CAN (par ex. "can0").
 * @return true si l’initialisation réussit, false sinon.
 */
bool
can_init (can_ctx_t *c, const char *ifname)
{
  if (!c || !ifname)
    return false;
  memset (c, 0, sizeof (*c));
  c->fd = -1;

  /* Création de la socket CAN brute */
  int fd = socket (PF_CAN, SOCK_RAW, CAN_RAW);
  if (fd < 0)
    {
      LOGE ("socket(PF_CAN): %s", strerror (errno));
      return false;
    }

  /* Recherche de l’index de l’interface */
  struct ifreq ifr;
  memset (&ifr, 0, sizeof (ifr));
  snprintf (ifr.ifr_name, sizeof (ifr.ifr_name), "%s", ifname);
  if (ioctl (fd, SIOCGIFINDEX, &ifr) < 0)
    {
      LOGE ("SIOCGIFINDEX(%s): %s", ifname, strerror (errno));
      close (fd);
      return false;
    }

  /* Ne pas recevoir nos propres trames (supprime les doublons) */
  int recv_own = 0;             // 0 = off
  if (setsockopt (fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &recv_own, sizeof (recv_own)) < 0)
    {
      LOGW ("setsockopt(CAN_RAW_RECV_OWN_MSGS): %s", strerror (errno));
    }

  /* Liaison de la socket à l’interface CAN */
  struct sockaddr_can addr;
  memset (&addr, 0, sizeof (addr));
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;

  if (bind (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
      LOGE ("bind CAN(%s): %s", ifname, strerror (errno));
      close (fd);
      return false;
    }

  /* Mode non bloquant */
  if (!set_nonblock (fd))
    {
      LOGW ("fcntl(O_NONBLOCK) failed (socket blocking) %c", 0);
    }

  /* Agrandir les buffers de réception/émission */
  int rcvbuf = 256 * 1024;
  (void) setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof (rcvbuf));
  int sndbuf = 256 * 1024;
  (void) setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof (sndbuf));

  c->fd = fd;
  return true;
}


/**
 * @brief Envoie une trame CAN standard de 8 octets.
 *
 * @param c : nontexte CAN actif.
 * @param can_id : identifiant CAN (11 bits).
 * @param data : tableau de 8 octets à envoyer.
 * @return true si l’envoi réussit, false sinon.
 */
bool
can_send (can_ctx_t *c, uint32_t can_id, const uint8_t data[8])
{
  if (!c || c->fd < 0)
    return false;

  struct can_frame f;
  memset (&f, 0, sizeof (f));
  f.can_id = can_id & CAN_SFF_MASK;   /**< ID standard sur 11 bits */
  f.can_dlc = 8;   /**< Longueur fixe : 8 octets */

  memcpy (f.data, data, 8);

  ssize_t n = write (c->fd, &f, sizeof (f));
  if (n < 0)
    {
      LOGE ("CAN write: %s", strerror (errno));
      return false;
    }
  return (n == (ssize_t) sizeof (f));
}

/**
 * @brief Lit les trames disponibles sur le bus CAN (mode non bloquant).
 *
 * Fonction appelée en boucle dans `my_loop()`.  
 * Elle récupère jusqu’à `max_frames` trames à chaque itération
 * et transmet les données décodées vers MQTT.
 *
 * Deux cas sont gérés :
 * - ID standard : trame directement connue dans la table.
 * - Mode tunnel : les deux premiers octets contiennent un "inner ID" à décoder.
 *
 * @param c : contexte CAN.
 * @param t : table de conversion topic/ID.
 * @param m  : contexte MQTT (pour republier).
 * @param max_frames : nombre maximum de trames à lire par itération.
 */
void
can_poll (can_ctx_t *c, const table_t *t, mqtt_ctx_t *m, int max_frames)
{
  if (!c || c->fd < 0)
    return;
  if (max_frames <= 0)
    max_frames = 8;

  fd_set rfds;
  struct timeval tv = (struct timeval)
  { 0, 0 };
  FD_ZERO (&rfds);
  FD_SET (c->fd, &rfds);

  /* Vérifie s’il y a des données à lire sans bloquer */
  int rv = select (c->fd + 1, &rfds, NULL, NULL, &tv);
  if (rv <= 0 || !FD_ISSET (c->fd, &rfds))
    return;

  for (int i = 0; i < max_frames; i++)
    {
      struct can_frame f;
      ssize_t n = read (c->fd, &f, sizeof (f));
      if (n < 0)
        {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          LOGW ("CAN read: %s", strerror (errno));
          /* Si l'interface a disparu / I/O error, fermer et demander ré-init externellement */
          if (errno == ENODEV || errno == ENETDOWN || errno == EIO || errno == EBADF)
            {
              LOGW ("CAN socket appears dead (errno=%d), cleaning up", errno);
              can_cleanup (c);
              return; /* retour vers la boucle principale qui pourra retenter can_init */
            }
          break;
        }
      if ((size_t) n != sizeof (f))
        break;

      /* 1) tentative d'identification par ID CAN */
      const entry_t *e = t ? table_find_by_canid (t, f.can_id) : NULL;
      const uint8_t *payload = f.data;
      uint8_t shifted[8];

      /* 2) Si echec, on tente le mode tunnel (ID dans les 2 premiers octets) */
      if (!e && f.can_dlc >= 2)
        {
          uint16_t inner_id = ((uint16_t) f.data[0] << 8) | f.data[1];
          e = table_find_by_canid (t, inner_id);
          if (e)
            {
              /* on “retire” les 2 octets d’en-tête et on recale le payload */
              memset (shifted, 0, sizeof (shifted));
              size_t copy = (f.can_dlc > 2) ? (size_t) (f.can_dlc - 2) : 0;
              if (copy > 6)
                copy = 6;       /* place restante (8-2) */
              memcpy (shifted, f.data + 2, copy);
              payload = shifted;
            }
        }
      /* 3) Si l’entrée correspond, on renvoie vers MQTT */
      if (!e)
        continue;
      (void) mqtt_handle_can_message (m, e, payload);
    }
}


/**
 * @brief Ferme proprement la socket CAN.
 * 
 * Libération mémoire CAN   
 *
 * @param c : contexte CAN à fermer.
 */
void
can_cleanup (can_ctx_t *c)
{
  if (!c)
    return;
  if (c->fd >= 0)
    {
      close (c->fd);
      c->fd = -1;
    }
}

// End of file
