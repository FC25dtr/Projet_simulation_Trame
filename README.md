#Projet IPC - Simulation d'un protocole de transport reseau réalisé en binome par Kelim et Tonin 

## Presentation generale du projet

L'architecture reproduit trois couches distinctes. Des processus clients generent des donnees et les envoient. Un processus transporteur joue le role de routeur : il verifie l'integrite des donnees et les achemine vers la bonne destination. Des processus serveurs ecoutent sur des ports et traitent les messages recus.

Le projet est decline en six versions. Chaque version conserve la meme logique generale mais remplace ou combine differemment les mecanismes IPC utilises pour la communication entre le transporteur et les serveurs. L'objectif pedagogique est de comparer les approches : tubes, files de messages System V, memoire partagee, et leurs combinaisons avec semaphores et synchronisation par drapeaux.

---

## Ce que fait le projet dans toutes ses versions

Au lancement, le programme prend deux arguments : le nombre de clients N et le nombre de serveurs M.

Chaque client porte un numero de port unique (de 1 a N). Il genere dix messages, calcule un checksum sur les donnees et les envoie au transporteur. Si le transporteur signale une erreur, le client retransmet jusqu'a cinq fois avant d'abandonner. Un semaphore (dans les versions qui traitent de grandes trames) empeche les clients d'ecrire simultanement dans le tube commun.

Le transporteur lit les messages des clients un par un. Pour chaque message, il recalcule le checksum pour verifier l'integrite, consulte un annuaire en memoire partagee pour savoir si un serveur ecoute sur le port demande, puis transmet les donnees au serveur par le mecanisme IPC propre a chaque version. Il renvoie au client un code indiquant si la transmission a reussi, si le checksum etait invalide, ou si le port etait ferme.

Les serveurs demarrent avec un delai aleatoire, choisissent un port libre dans l'annuaire et attendent les messages. A la reception, ils verifient le checksum et affichent un compte-rendu. Ils se terminent proprement quand le transporteur leur signale la fin du programme.

---

## Vue d'ensemble des six versions

**Version 1** : grandes trames (8 Mo), tube pour le transport vers les serveurs, semaphore mutex pour les clients.

**Version 2** : petites trames (2000 entiers), tube pour le transport vers les serveurs, pas de semaphore (les ecritures sont atomiques a cette taille).

**Version 3** : petites trames, files de messages System V pour le transport vers les serveurs, pas de semaphore.

**Version 4** : grandes trames (8 Mo), files de messages System V avec decoupage en chunks, semaphore mutex pour les clients.

**Version 5** : petites trames, second segment de memoire partagee avec drapeaux de synchronisation pour le transport vers les serveurs, pas de semaphore.

**Version 6** : grandes trames (8 Mo), second segment de memoire partagee avec drapeaux et memcpy, semaphore mutex pour les clients.

---

## Modifications necessaires pour compiler et executer chaque version

Toutes les versions se compilent avec la meme commande de base :

```bash
gcc -Wall -o programme main.c
```

Et se lancent avec :

```bash
./programme {num_client} {num_server}
```

Les paragraphes suivants detaillent les points d'attention specifiques a chaque version.

---

### Version 1 - Grandes trames par tube

Cette version utilise un tube anonyme pour envoyer les donnees du transporteur aux serveurs. Les donnees (8 Mo) sont decoupees en blocs de 4096 entiers pour ne pas depasser la capacite du tube. Un semaphore protege le tube commun entre clients.

Aucune modification n'est necessaire pour la compilation. En revanche, le programme alloue dynamiquement 8 Mo par client et 8 Mo pour le transporteur. Avec un N eleve, la consommation memoire peut devenir importante. Il faut egalement s'assurer que le fichier /tmp existe sur la machine car ftok l'utilise pour generer les cles IPC.

Un seul tube relie le transporteur a chaque serveur. Le transporteur doit donc fermer ces tubes proprement apres avoir traite tous les messages pour que les serveurs detectent la fin de flux et se terminent.

---

### Version 2 - Petites trames par tube

Cette version fonctionne avec des trames de 2000 entiers (environ 8 ko). A cette taille, une ecriture dans le tube est atomique sous Linux (PIPE_BUF vaut 64 ko), ce qui rend le semaphore inutile. Chaque trame est envoyee en une seule operation write.

Aucune modification n'est necessaire. C'est la version la plus simple a faire fonctionner. L'architecture est par ailleurs robuste face aux erreurs d'utilisation : si le nombre de serveurs M dépasse le nombre de ports disponibles N, la logique d'attribution l'anticipe. Grâce à une vérification stricte du nombre d'essais, les serveurs excédentaires s'éteindront proprement après avoir scanné l'annuaire après N boucles, évitant ainsi toute boucle infinie et préservant les ressources du processeur. 

La condition `tentative < N` est placée en première position pour éviter d'accéder à une zone mémoire hors du tableau. Le langage C utilisant une évaluation paresseuse, la seconde partie de la condition n'est pas testée si la première est fausse, garantissant ainsi la sécurité de l'accès mémoire.

```c
while (tentative < N && tabPorts[randPort-1] != -1) { 
        randPort = rand() % N + 1;
        tentative++; 
    }
    if (tentative >= N) {
        exit(1);
    }
```

---

### Version 3 - Petites trames par file de messages

Cette version remplace les tubes transport-serveur par des files de messages System V. Une file est creee pour chaque port. Le transporteur y depose les paquets avec msgsnd et les serveurs les recuperent avec msgrcv.

Pour que cette version fonctionne, il faut verifier que la limite systeme sur la taille maximale d'un message dans une file (MSGMAX) est suffisante pour contenir un paquetServeur.

Les files de messages persistent dans le systeme apres la fin du programme si elles ne sont pas supprimees explicitement. En cas de crash, des files orphelines peuvent subsister. Pour les lister et les supprimer manuellement :

```bash
ipcs -q
ipcrm -q 
```

---

### Version 4 - Grandes trames par file de messages

Cette version combine les grandes trames de 8 Mo avec les files de messages. Comme un message dans une file ne peut pas depasser MSGMAX (generalement 8 ko), les 8 Mo sont decoupes en chunks de 1024 entiers (4096 octets chacun). Le transporteur envoie 1954 chunks de donnees suivis d'un message de checksum. Le serveur reconstitue le message en lisant les chunks un par un.


La limite MSGMAX doit etre superieure ou egale a 4096 octets (taille d'un chunk). La limite MSGMNB doit etre suffisamment grande pour stocker l'ensemble des chunks d'un message complet en attente.

Comme pour la version 3, les files orphelines doivent etre nettoyees manuellement en cas de crash.

---

### Version 5 - Petites trames par memoire partagee avec drapeaux

Cette version utilise un second segment de memoire partagee a la place des tubes ou files de messages. Chaque case du tableau tabMsg contient un paquetServeur de 2000 entiers et un drapeau d'etat. La synchronisation repose sur de l'attente active : le transporteur et le serveur lisent le drapeau en boucle jusqu'a ce qu'il prenne la valeur attendue.

Aucune modification de parametres systeme n'est necessaire. La memoire partagee utilisee est de taille N fois la taille d'une case shmTab, ce qui reste modeste avec des trames de 2000 entiers.

Il faut en revanche etre conscient que l'attente active consomme du temps processeur, en particulier du cote du serveur qui boucle avec une pause de 100 millisecondes. Cette approche n'offre pas les garanties d'un semaphore et peut produire des comportements imprevisibles si le systeme est tres charge ou si l'ordonnanceur suspend un processus au mauvais moment.

En cas de crash, les segments de memoire partagee peuvent persister. Pour les lister et les supprimer :

```bash
ipcs -m
ipcrm -m 
```

---

### Version 6 - Grandes trames par memoire partagee avec drapeaux et memcpy

Cette version est la combinaison la plus lourde : grandes trames de 8 Mo, semaphore mutex, et second segment de memoire partagee ou chaque case contient directement un tableau de deux millions d'entiers.

Comme pour la version 5, les segments orphelins doivent etre supprimes manuellement en cas de crash. Le transfert des donnees utilise memcpy, ce qui est efficace mais implique que les 8 Mo sont copiés deux fois : une premiere fois du tube vers le tampon du transporteur, une seconde fois du tampon vers la memoire partagee.

---


## Dependances communes

Toutes les versions utilisent uniquement la bibliotheque standard C et les appels systeme POSIX. Aucune installation de bibliotheque tierce n'est necessaire.

- unistd.h : pipe, read, write, fork, sleep, usleep
- sys/wait.h : wait
- sys/ipc.h : ftok, IPC_CREAT, IPC_RMID
- sys/shm.h : shmget, shmat, shmdt, shmctl (toutes les versions)
- sys/sem.h : semget, semctl, semop (versions 1, 4, 6)
- sys/msg.h : msgget, msgsnd, msgrcv, msgctl (versions 3, 4)
- string.h : memcpy (version 6)
