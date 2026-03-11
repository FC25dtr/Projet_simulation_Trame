#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>   // inclusion de la bibliothèque pour les files de messages System V

union semun { //union necessaire pour l'utilisation des semaphores
    int val;               // SETVAL
    struct semid_ds *buf;  // IPC_STAT, IPC_SET
    unsigned short *array; // SETALL, GETALL
};

unsigned short calcul_checksum(unsigned short ports, unsigned short Nmsg, unsigned int *donnees){
    unsigned int sum = 0;                    //creation d'une somme sur 32 bits avant de mettre le checksum pour pas de depassement
    sum = ports + Nmsg;                      //initialisation du checksum avec port plus Nmsg
    for(int k = 0; k < 2000000; k++){        //boucle pour le placement des données dans la trame
        sum += (donnees[k] & 0xFFFF);        //ajout des données à la somme, 0xffff met tous les bits qu'on ne veut pas à 0
        sum += (donnees[k] >> 16) & 0xFFFF;  // decale pour prendre les bits qu'on veut plus met les autres à 0
    }
    while(sum >> 16) {                       //cette boucle permet de remettre la somme sur 16 bits
        sum = (sum & 0xFFFF) + (sum >> 16);  // le decalge de 16 ajoute le depassement au debut du checksum
    }
    return (unsigned short)(~sum & 0xFFFF);  //inversion des bits (complément à 1) et placement dans checksum je force le 16 bits pour eviter les depassements
}

struct Trame{ //creation de la trame
    unsigned short ports;    //definition du port sur 16 bits
    unsigned short Nmsg;     //definition du numéro de message sur 16 bits
    unsigned int *donnees;   //données de 2 000 000 d'entier donc un pointeur pour allouer dynamiquement
    unsigned short checksum; //checksum sur 16 bits
};
typedef struct Trame trame;

#define BLOCK 4096          //pour ne pas depasser la taille du tube qui est environ 64 000
#define MSG_CHUNK 1024      //taille d'un chunk en nombre d'unsigned int (1024 * 4 = 4096 octets, limite sure pour msgsnd)
#define MSG_TYPE_DATA  1    //type utilisé pour les chunks de données dans la file
#define MSG_TYPE_CHECK 2    //type utilisé pour le message contenant le checksum final

// Structure d'un message chunk envoyé dans la file System V
struct msgbuf_chunk {
    long mtype;                   //type du message obligatoire pour msgsnd/msgrcv
    unsigned int data[MSG_CHUNK]; //un chunk de 1024 unsigned int = 4096 octets
};

// Structure d'un message checksum envoyé dans la file System V
struct msgbuf_check {
    long mtype;              //type du message obligatoire pour msgsnd/msgrcv
    unsigned short checksum; //checksum calculé par le transporteur pour vérification
};

int main(int argc, char* argv[]){ // argv[1] = NB clients argv[2] = NB server
    if (argc < 3) { //gestion si l'utilisateur ne place pas de paramètre
        fprintf(stderr, "Usage: %s <nb_clients> <nb_servers>\n", argv[0]);
        exit(1);
    }

    union semun arg;  //creation de l'union
    arg.val = 1;      //permettra l'initialisation du semaphore mutex
    key_t key = ftok("/tmp", 123); //permet la création d'une clé pour la création des semaphores

    if(key == -1){ //gestions des erreurs de ftok
        perror("ftok");
        exit(0);
    }

    int semid = semget(key, 1, IPC_CREAT | 0644); //ceation du groupe de semaphore (1 seul) avec les droits

    if(semid == -1){ //gestion des erreurs semget
        perror("semget");
        exit(0);
    }

    semctl(semid, 0, SETVAL, arg); //affectation de la valeur de arg au semaphore ici 1

    struct sembuf *sops = (struct sembuf*)malloc(sizeof(struct sembuf)); //structure pour les opérations sur mon semaphore
    sops->sem_num = 0;  /* Agir sur le sémaphore 0 */
    sops->sem_op  = -1; /* decremente de 1 */
    sops->sem_flg = 0;

    int i = 1;              //initialisation des clients (numéro de ports)
    int j = 1;              //initialisation du nombre de message par clients
    int N = atoi(argv[1]); //creation de la variable de N processus
    int M = atoi(argv[2]); //creation de la variable de M server

    pid_t pidClient[N];  //création du tableau de PID de tous les clients
    int tube2[N][2];     //creation des tubes retour transport -> client
    for(int o = 0; o < N; o++){
        pipe(tube2[o]); //creation de touts les tubes retour
    }

    int tube[2];
    if(pipe(tube) == -1){
        perror("pipe"); //gestion d'erreur
        exit(0);
    } //creation du tube qui reliera les clients au transporteur

    // Création de la zone mémoire de taille N codé sur 2 octets (sizeof(short)) avec les droits de lecture ecriture USER (O600)
    int shmid = shmget(key, N * (sizeof(short)), IPC_CREAT | 0600);
    if (shmid == -1) {
        perror("ERREUR shmget");
        exit(1);
    }

    short* tabPorts = shmat(shmid, NULL, 0); //on attache la mémoire partagé
    if (tabPorts == (void *) -1) {           // shmat renvoie un pointeur void*
        perror("ERREUR shmat");              //controle d'erreur
        exit(1);
    }

    for (int i = 0; i < N; i++) {
        tabPorts[i] = -1; // Initialisation de touts les elements du tableau a -1 <=> ports libre
    }

    // Création des files de messages System V, une par port (remplace les tubeVersServ)
    int mqVersServ[N]; //tableau des identifiants de files de messages, une par port
    for (int port = 0; port < N; port++) {
        key_t keyMq = ftok("/tmp", port + 1); //clé unique par port basée sur son numéro (différente de la clé sémaphore)
        if (keyMq == -1) { //gestion erreur ftok
            perror("ftok mq");
            exit(1);
        }
        mqVersServ[port] = msgget(keyMq, IPC_CREAT | 0644); //creation de la file avec droits lecture/ecriture
        if (mqVersServ[port] == -1) { //gestion erreur msgget
            perror("msgget");
            exit(1);
        }
    }

/*<============================================SERVEURS==========================================================>*/
    pid_t pidServ[M];

    for (int i = 1; i <= M; i++) {
        pidServ[i-1] = fork();

        if (pidServ[i-1] == -1) {
            perror("ERREUR fork serveur"); //gestion erreur fork
            exit(1);
        }

        if (pidServ[i-1] == 0) { //action réalisé par les fils
            int tentative = 0;
            int randPort;
            int nb_traites = 0;
            int nb_erreurs_checksum = 0;

            close(tube[0]); // Le serveur ne lit pas les clients
            close(tube[1]); //  Le serveur n'écrit pas au transporteur
            for(int k = 0; k < N; k++) {
                close(tube2[k][0]);
                close(tube2[k][1]);
            }

            srand(123456 + i); // On s'assure que la seed est unique
            sleep(rand() % 6);

            // Recherche d'un port libre dans la mémoire partagée
            randPort = rand() % N + 1;
            while (tentative < N && tabPorts[randPort-1] != -1) { 
                randPort = rand() % N + 1;
                tentative++; //si occupé on en essaye un autre
            }
			if (tentative >= N) {
				exit(1);
			}
            tabPorts[randPort-1] = i; // On réserve le port avec le numéro du serveur

            printf("[SERVEUR %d] : J'écoute sur le port %d\n", i, randPort);

            // PRÉPARATION DE LA MÉMOIRE (8 Mo)
            unsigned int *buffer = (unsigned int*)malloc(sizeof(unsigned int) * 2000000);
            if (!buffer) {
                perror("malloc serveur");
                exit(1);
            }

            struct msgbuf_chunk msgChunk; //structure de reception des chunks de données
            struct msgbuf_check msgCheck; //structure de reception du checksum final

            // BOUCLE DE RÉCEPTION PAR CHUNKS VIA LA FILE DE MESSAGES
            while (1) {
                int total_lu = 0; //compteur d'unsigned int reçus au total pour ce message

                // Lecture des chunks de données jusqu'à reconstituer les 2 millions d'entiers
                while (total_lu < 2000000) {
                    int nb_attendus = MSG_CHUNK; //nombre d'entiers attendus dans ce chunk
                    if (total_lu + MSG_CHUNK > 2000000) //ajustement pour le dernier chunk plus petit
                        nb_attendus = 2000000 - total_lu;

                    // Réception bloquante du prochain chunk de données
                    ssize_t ret = msgrcv(mqVersServ[randPort-1], &msgChunk, sizeof(unsigned int) * nb_attendus,MSG_TYPE_DATA, 0); //0 = bloquant, attend un message de type DATA
                    if (ret == -1) { 
                        //si la file est détruite (nettoyage du pere) on sort proprement
                        printf("[BILAN FINAL SERVEUR %d - Port %d]\n", i, randPort);
                        printf("  - Total messages consommés : %d\n", nb_traites);
                        printf("  - Erreurs checksum : %d\n", nb_erreurs_checksum);
                        free(buffer);
                        exit(0);
                    }
                    // Copie du chunk reçu dans le buffer de reconstitution
                    for (int x = 0; x < nb_attendus; x++)
                        buffer[total_lu + x] = msgChunk.data[x]; //recopie des entiers dans le buffer
                    total_lu += nb_attendus; //mise à jour du compteur
                }

                // Lecture du message checksum qui arrive après tous les chunks (type CHECK)
                if (msgrcv(mqVersServ[randPort-1], &msgCheck,sizeof(unsigned short),MSG_TYPE_CHECK, 0) == -1) {
                    free(buffer);
                    exit(0);
                }

                nb_traites++;

                // VÉRIFICATION du checksum (recalculé avec ports=0 et Nmsg=0 comme convenu avec le transporteur)
                unsigned short checksum_calc = calcul_checksum(0, 0, buffer);

                if (checksum_calc == msgCheck.checksum) {
                    printf("[SERVEUR %d] Message de 8Mo reçu intact sur port %d ! Data[0] = %u\n", i, randPort, buffer[0]);
                } else {
                    nb_erreurs_checksum++;
                    printf("[SERVEUR %d] Erreur de checksum sur le port %d...\n", i, randPort);
                }

                // Attente entre 0 et 4 secondes avant la prochaine lecture (Consigne)
                sleep(rand() % 5);
            }
            // NETTOYAGE
            free(buffer);
            printf("[SERVEUR %d] Fin de service.\n", i);
            exit(0);
        }
    }


/*<============================================CLIENT==========================================================>*/

    for(i = 1; i <= N; i++){ // boucle de création des fils (processus clients)
        pidClient[i-1] = fork(); //creation des fils avec fork
        if(pidClient[i-1] == -1){ //gestion d'erreur de fork
            perror("fork");
            exit(0);
        }

        if(pidClient[i-1] == 0){ //operations seulement réalisé par les fils
            //pas besoin de refaire semid le fils herite des attributs du pere
            close(tube[0]); //fermeture descripteur lecture
            srand(123456 + i); //genetation de la graine d'aléa
            for(int k = 0; k < N; k++){
                if(k != i-1) { // ferme tous les tubes des autres clients
                    close(tube2[k][0]);
                    close(tube2[k][1]);
                }
            }
            close(tube2[i-1][1]); // on ferme l'ecriture car on veut seulement lire dessus

            int msg_envoyes = 0;
            int msg_perdus = 0;
            
            for(j = 0; j < 10; j++){ //créations des 10 messages par server
                trame T;  //creation d'une structure trame
                T.donnees = (unsigned int*)malloc(sizeof(unsigned int) * 2000000); //allocatio d'un espace mémoire
                if (!T.donnees) { //gestion des erreurs d'allocation
                    perror("malloc");
                    exit(0);
                }

                for (int k = 0; k < 2000000; k++) T.donnees[k] = rand(); //creation des données aléatoire 

                T.ports = i; //placement du port (numéro du fils)
                T.Nmsg = j; // placement du numéro de message
                T.checksum = calcul_checksum(T.ports, T.Nmsg, T.donnees); // appel de la fonction checksum

                int succes = 0; // Pour sortir de la boucle de retransmission
                int timeout_count = 0;

                while (!succes && timeout_count < 5){
                    sops->sem_op = -1;        //au cas ou l'operande est à +1
                    semop(semid, sops, 1);    //je prend le mutex pour etre le seul a écrire
                    printf("[CLIENT %d] Je prend la possession du tube pour envoyer ma trame\n", T.ports);
                    printf("[CLIENT %d] Envoi du message numéro %d par le port %d\n", i, j, T.ports);
                    write(tube[1], &T.ports, sizeof(T.ports)); //envoi du ports
                    write(tube[1], &T.Nmsg,  sizeof(T.Nmsg));  //envoi du numéro de message

                    for (int k = 0; k < 2000000; k += BLOCK) { //envoi de toute la trame de donnée par block
                        int chunk = BLOCK;              //definition de la taille
                        if (k + BLOCK > 2000000){       //cas ou inférieur a block
                            chunk = 2000000 - k;        // dernier bloc plus petit
                        }
                        size_t size = chunk * sizeof(int); //combien on veut envoyer
                        int total = 0;                   //combien d'octet deja envoyé
                        char *ptr = (char*)&T.donnees[k]; //char permet d'avancer precisement de 1 octet à la fois

                        while (total < size) { //tant qu'on a pas envoyé tout les octets
                            int n = write(tube[1], ptr + total, size - total); //on ecris ce qu'il reste
                            if (n <= 0) {
                                perror("write");
                                exit(1);
                            }
                            total += n;
                        }
                    }
                    write(tube[1], &T.checksum, sizeof(T.checksum)); //envoi du checksum

                    sops->sem_op = 1;      //modification de l'opération du sémaphore
                    semop(semid, sops, 1); //je rend le mutex pour laisser la place a quelqu'un

                    int test = 0;
                    read(tube2[i-1][0], &test, sizeof(int));

                    switch(test) {
                        case 0:
                            succes = 1; // On passe au message suivant
                            break;
                        case 1:
                            printf("[CLIENT %d] Erreur Checksum, re-tentative...\n", i);
                            break;
                        case 2:
                            timeout_count++;
                            printf("[CLIENT %d] Port fermé (Essai %d/5), attente...\n", i, timeout_count);
                            if (timeout_count < 5) { // On ne dort pas si c'était la dernière chance, optimisation
                                sleep(rand() % 3 + 1);
                            }
                            break;
                    }
                }

                free(T.donnees); //liberation de l'espace memoire
                if (succes) {
                    msg_envoyes++;
                    printf("[CLIENT %d] Succès message %d\n", i, j);
                    sleep(rand() % 6); //endors le fils pendant un temps tps
                } else {
                    // Si on est ici, c'est que timeout_count vaut 5 et succes vaut 0.
                    msg_perdus++;
                    printf("----> [CLIENT %d] Abandon après 5 tentatives infructueuses. TERMINAISON.\n", i);
                    printf("[BILAN FINAL CLIENT %d - ÉCHEC]\n", i);
                    printf("  - Messages réussis : %d/10\n", msg_envoyes);
                    printf("  - Messages abandonnés : %d/10\n", msg_perdus);
                    close(tube2[i-1][0]); // On ferme proprement
                    exit(0); // LE CLIENT SE TERMINE ICI COMME DEMANDÉ
                }
            } //quand j'arrive la j'ai envoyé tous les message je peut donc attendre le controle de chacun
            close(tube2[i-1][0]); // fermeture lecture après avoir reçu toutes les confirmations
            printf("[BILAN FINAL CLIENT %d - SUCCÈS]\n", i);
            printf("  - Messages réussis : %d/10\n", msg_envoyes);
            printf("  - Messages abandonnés : %d/10\n", msg_perdus);
            exit(0); //fermeture du client quand il a terminé d'envoyer les 10 messages
        }
    }

    close(tube[1]); //le transporteur ferme l'ecriture car il n'en aura pas besoin apres boucle pour ne pas casser le tube

/*<===========================================TRANSPORTEUR==================================================>*/

    trame T2; //creation de la zone ou seront récupéré les trames pour controle
    T2.donnees = (unsigned int*)malloc(sizeof(unsigned int) * 2000000); //Allocation de la mémoire

    struct msgbuf_chunk msgChunk; //structure réutilisée pour chaque envoi de chunk vers le serveur
    struct msgbuf_check msgCheck; //structure réutilisée pour chaque envoi de checksum vers le serveur
    msgChunk.mtype = MSG_TYPE_DATA;  //le type data ne change jamais pour les chunks
    msgCheck.mtype = MSG_TYPE_CHECK; //le type check ne change jamais pour le checksum

    while (read(tube[0], &T2.ports, sizeof(unsigned short)) > 0) { //boucle qui liera les 10 messages des N fils

        // Lecture de l'entête suite
        read(tube[0], &T2.Nmsg, sizeof(unsigned short)); //lecture du numéro de message
        printf("[TRANSPORTEUR]reception du message avec numéro de ports %d et numéro de message %d \n", T2.ports, T2.Nmsg);

        for (int k = 0; k < 2000000; k += BLOCK) {
            int chunk = BLOCK;           //definition de la taille
            if (k + BLOCK > 2000000){    //cas ou inférieur a block
                chunk = 2000000 - k;     // dernier bloc plus petit
            }
            size_t size = chunk * sizeof(unsigned int);
            int total   = 0;
            char *ptr   = (char*)&T2.donnees[k];

            while (total < size) { //pareil on force l'envoi de toutes les données avec une boucle pour eviter les pertes
                int n = read(tube[0], ptr + total, size - total);
                if (n <= 0) {
                    perror("read client->transport");
                    exit(1);
                }
                total += n;
            }
        }

        // Lecture checksum
        read(tube[0], &T2.checksum, sizeof(T2.checksum));

        int valReponse = 0; //variable de reussite

        // VERIFICATION DE L'ANNUAIRE (MÉMOIRE PARTAGÉE)
        if(calcul_checksum(T2.ports, T2.Nmsg, T2.donnees) == T2.checksum){
            printf("[TRANPORTEUR]le checksum de %d numéro %d est juste\n", T2.ports, T2.Nmsg);

            if (tabPorts[T2.ports-1] != -1) {
                // Port ouvert : envoi des données au serveur via la file de messages par chunks
                printf("[TRANSPORTEUR] Port %d OK -> Transfert des 8Mo au serveur via file de messages.\n", T2.ports);

                int envoye = 0; //compteur d'unsigned int déjà envoyés dans la file
                while (envoye < 2000000) {
                    int nb = MSG_CHUNK;                    //nombre d'entiers à envoyer dans ce chunk
                    if (envoye + MSG_CHUNK > 2000000)      //ajustement pour le dernier chunk plus petit
                        nb = 2000000 - envoye;

                    // Remplissage du chunk avec les données correspondantes
                    for (int x = 0; x < nb; x++)
                        msgChunk.data[x] = T2.donnees[envoye + x]; //copie des données dans le message

                    // Envoi du chunk dans la file du port destinataire (bloquant si file pleine)
                    if (msgsnd(mqVersServ[T2.ports-1], &msgChunk,sizeof(unsigned int) * nb,0) == -1) { //0 = bloquant, on attend que la file ait de la place
                        perror("msgsnd data");
                        exit(1);
                    }
                    envoye += nb; //mise à jour du compteur d'envoi
                }

                // Envoi du checksum recalculé (avec ports=0 et Nmsg=0 comme convenu avec le serveur)
                msgCheck.checksum = calcul_checksum(0, 0, T2.donnees);
                if (msgsnd(mqVersServ[T2.ports-1], &msgCheck,
                           sizeof(unsigned short),
                           0) == -1) { //0 = bloquant
                    perror("msgsnd checksum");
                    exit(1);
                }

            } else {
                valReponse = 2;
                printf("[TRANSPORTEUR] Port %d FERMÉ -> Information client.\n", T2.ports);
            }
        } else {
            // ERREUR checksum
            valReponse = 1;
            printf("[TRANSPORTEUR]erreur sur le checksum port=%d msg=%d\n", T2.ports, T2.Nmsg);
        }
        write(tube2[T2.ports-1][1], &valReponse, sizeof(int));
        sleep(rand() % 3 + 1); //attend 1 à 3 secondes
    }

/*<===============================NETTOYAGE================================================>*/
    for(int k = 0; k < N; k++){ //fermeture de tout les descripteur de tube2 et suppression des files
        close(tube2[k][1]);
        msgctl(mqVersServ[k], IPC_RMID, NULL); //suppression de la file de messages du port k
    }

    close(tube[0]); //fermeture de la lecture de tube

    while(wait(NULL) > 0);

    semctl(semid, 0, IPC_RMID); //supression du sémaphore
    free(sops);       // on libère l'espace alloué a sops pendant l'execution
    free(T2.donnees); //pareil

    // Destruction de la mémoire partagée
    shmdt(tabPorts);
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl");
    } else {
        printf("Nettoyage mémoire partagée OK. Fin du programme.\n");
    }

    printf("\n \n[SYSTEME] Nettoyage terminé tout est OK\n \n ");
}
