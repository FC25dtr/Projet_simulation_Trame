#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>

// Définition des drapeaux (flags) pour la synchronisation de la mémoire partagée
#define PLEIN 1
#define VIDE 0


// Structure de la donnée utile destinée au Serveur
typedef struct paquetServeur {
    unsigned int donnees[2000]; // Payload de 2000 entiers
    unsigned short checksum;    // Somme de contrôle pour valider l'intégrité
} paquetServeur;

typedef struct SHMTAB {
    paquetServeur message; // Le paquet déposé par le transporteur
    int etat;              // Le drapeau de synchronisation (PLEIN ou VIDE)
} shmTab;

typedef struct Trame { //creation de la trame 
    unsigned short ports; //definition du port sur 16 bits
    unsigned short Nmsg; //definition du numéro de message sur 16 bits
    paquetServeur paquet; // Permet d'imbriquer la 2eme structure => gains de performances, memoire, lisibilité...
} trame;


unsigned short calculerChecksum(unsigned int* tab, int taille, unsigned short p, unsigned short n) {
    unsigned int sum = p + n;
    for(int k =0; k<2000; k++){ //on refait la meme boucle que pour le calcul du checksum  
            sum += (tab[k] & 0xFFFF); //ajout des données à la somme, 0xffff met tous les bits qu'on ne veut pas à 0 
            sum += (tab[k] >>16) & 0xFFFF; // decale pour prendre les bits qu'on veut plus met les autres à 0
            }
                
        while(sum >> 16) {//cette boucle permet de remettre la somme sur 16 bits
            sum = (sum & 0xFFFF) + (sum >> 16); // le decalge de 16 ajoute le depassement au debut du checksum
            }
    return (unsigned short)(~sum & 0xFFFF);
}

int main(int argc, char* argv[]){ // argv[1] = NB clients argv[2] = NB server
    if (argc < 3) { //gesion du problème de nombre de paramètre
        fprintf(stderr, "[ERREUR] : la commande requiert 2 arguments : %s <nb clients> <nb_server>\n", argv[0]);
        exit(1); 
    }
    int i = 1; // Initialisation des clients (numéro de ports)
    int j = 1;  // Initialisation du nombre de message par clients 
    int N = atoi(argv[1]); // Creation de la variable de N processus 
    int M = atoi(argv[2]); // Creation de la variable de M server 

    // -----------------------------------------------------------------------------------
    // 1. MÉMOIRE PARTAGÉE 1 : L'ANNUAIRE DES PORTS (tabPorts) sur 2 octets sizeof(short)
    // Sert à savoir quel serveur écoute sur quel port.
    key_t key = ftok("/tmp", 30);
    int shmidTabPort = shmget(key, N*(sizeof(short)), IPC_CREAT | 0600);
    if (shmidTabPort == -1 ) {
        perror("ERREUR shmget tabport");
        exit(1);
    }
    
    short* tabPorts = shmat(shmidTabPort, NULL, 0); //on attache la mémoire partagé 
    if (tabPorts == (void *) -1 ) { // shmat renvoie un pointeur void*
        perror("ERREUR shmat tabPort"); //controle d'erreur 
        exit(1);
    }

    // -----------------------------------------------------------------------------------
    // 2. MÉMOIRE PARTAGÉE 2 : tabMsg
    // Remplace les tubes pour l'envoi des paquets du Transporteur aux Serveurs.    
    key = ftok("/tmp", 40);
    int shmidTabMsg = shmget(key, N*sizeof(shmTab), IPC_CREAT | 0600) ;
    if (shmidTabMsg == -1 ) {
        perror("ERREUR shmget tabMsg");
        exit(1);
    }

    shmTab* tabMsg = shmat(shmidTabMsg, NULL, 0); // Attachement en tant que pointeur typé (shmTab*)
    if (tabMsg == (void *) -1 ) { // shmat renvoie un pointeur void*
        perror("ERREUR shmat tabMsg");  
        exit(1);
    }

    // Initialisation : tabMsg VIDE au départ
    for (int k = 0; k < N; k++) {
        tabMsg[k].etat = VIDE;
    }

    // Initialisation de l'annuaire : tous les ports sont libres (-1) au départ
    for (int i = 0; i < N; i++) {
        tabPorts[i] = -1; 
    }
    
    // Tubes pour les ACK : Transporteur -> Clients
    int tube_ACK[N][2];
    for(int o = 0 ; o < N ; o++){
        pipe(tube_ACK[o]); // Creation de touts les tubes TRANSPORT -> CLIENT
    }

    int tube_client_transport[2]; // CREATION DU TUBE CLIENT -> TRANSPORT
    
    if(pipe(tube_client_transport) == -1){
            perror("pipe");
            exit(0);
    }

    /*--------------------------SERVEURS--------------------------*/
    pid_t pidServ[M]; //tableau de pid des servers
    for (int i = 1; i <= M; i++) {
        
        pidServ[i-1] = fork(); //creation des servers (fils)
        

        if (pidServ[i-1] == -1) {
            perror("ERREUR fork serveur");
            exit(1);
        }

        // ---------------------------------------------------------------------
        // CODE EXÉCUTÉ UNIQUEMENT PAR LE PROCESSUS FILS (SERVEUR)
        if (pidServ[i-1] == 0) { //action réalisé par les fils

            int randPort;
            int tentative = 0;

            // Statistiques serveur
            int nb_traites = 0;
            int nb_erreurs_checksum = 0;
            
            // 1. Fermeture des descripteurs de tubes inutilisés
            close(tube_client_transport[0]);
            close(tube_client_transport[1]); 

            for(int k=0; k<N; k++) {  // Fermeture des tubes d'ACK (réservés au Transporteur->Client)
                close(tube_ACK[k][0]); 
                close(tube_ACK[k][1]); 
            }

            // 2. INITIALISATION ET ENREGISTREMENT DANS L'ANNUAIRE
            srand(123456+i); // On s'assure que la seed est unique
            sleep(rand() % 6);// Attente aléatoire entre 0 et 5 secondes avant le lancement du server

            randPort = rand() % N + 1; // Connection a un port aleatoire au plus N
            
            // Recherche d'une case libre (-1) dans la mémoire partagée "tabPorts"
            while (tentative < N && tabPorts[randPort-1] != -1) {
                randPort = rand() % N + 1;
                tentative++; //si occupé on en essaye un autre
            }
			if (tentative >= N) {
				exit(1);
			}

            // Enregistrement : on s'approprie le port en y écrivant notre ID de serveur (i)
            tabPorts[randPort-1] = i; 
            printf("[SERVEUR %d] : J'écoute sur le port %d\n", i, randPort);
            
            // ---------------------------------------------------------------------
            // 3. BOUCLE DE TRAITEMENT
            // Le serveur surveille sa propre case (randPort-1) dans tabMsg
            // L'état '2' est le signal envoyé par le Transporteur pour terminer les processus serveur
            while (tabMsg[randPort-1].etat != 2) {
                
                // Si l'état est PLEIN (1), le Transporteur a déposé un nouveau paquet pour nous    
                if (tabMsg[randPort-1].etat == PLEIN) {
                    nb_traites++; // Un message est arrivé
                    // Vérification de l'intégrité de la donnée via le Checksum
                    if (calculerChecksum(tabMsg[randPort-1].message.donnees, 2000, 0, 0) == tabMsg[randPort-1].message.checksum) { //Si checksum juste
                        printf("[SERVEUR %d] Message reçu avec succès sur le port %d !\n", i, randPort);
                        printf("[SERVEUR %d] '%u' \n", i, tabMsg[randPort-1].message.donnees[0]);
                    } 
                    else {
                        nb_erreurs_checksum++;
                        printf("[SERVEUR %d] Erreur de checksum sur le port %d...\n", i, randPort);
                    }
                    // On repasse l'état à VIDE pour dire au Transporteur que la case de tabMsg est libre
                    tabMsg[randPort-1].etat = VIDE;

                    // Attendre avant la prochaine lecture
                    sleep(rand() % 5);
                }
                // Si l'état est VIDE (0), on attend un court instant avant de revérifier
                // Evite de reboucler directement si le transporteur est entrain d'ecrire
                else usleep(100000);
                
            }           
            // Affichage du bilan serveur à la fermeture
            printf("[BILAN FINAL SERVEUR %d - Port %d]\n", i, randPort);
            printf("  - Total messages consommés : %d\n", nb_traites);
            printf("  - Erreurs checksum : %d\n", nb_erreurs_checksum);
            exit(0); // Fermeture des fils 
        } 
    }

    pid_t pidClient[N]; //création du tableau de PID de tous les clients
    
    /*--------------------------CLIENTS--------------------------*/
    for(i = 1 ; i <= N ; i++){ // boucle de création des fils (processus clients)
        pidClient[i-1] = fork(); //creation des fils avec fork
        if(pidClient[i-1] == -1){//gestion d'erreur de fork
            perror("fork");
            exit(1);
        }
        
        if(pidClient[i-1] == 0){ //operations seulement réalisé par les fils 
            close(tube_client_transport[0]); //fermeture descripteur lecture
            srand(123456+i); //genetation de la graine d'aléa
            for(int k = 0; k < N; k++){
                if(k != i-1) { 
                    close(tube_ACK[k][0]);
                    close(tube_ACK[k][1]);
                }
            }
            close(tube_ACK[i-1][1]); // on ferme l'ecriture car on veut seuelement lire dessus 

            int msg_envoyes = 0; // Pour les statistiques
            int msg_perdus = 0;
            
            for(j=0; j<10; j++){ //créations des 10 messages par server
                trame T;
                T.ports = i;
                T.Nmsg = j;

                int succes = 0; // Pour sortir de la boucle de retransmission
                int timeout_count = 0;


                //boucle pour le placement des données dans la trame 
                for(int k =0; k<2000; k++) T.paquet.donnees[k] = rand();
                T.paquet.checksum = calculerChecksum(T.paquet.donnees, 2000, T.ports, T.Nmsg);

                
                // timeout pour gerer les 5 tentatives des clients
                while(!succes && timeout_count < 5) { 
                    printf("[CLIENT %d] Envoi message %d\n", i, j);
                    write(tube_client_transport[1], &T, sizeof(trame));

                    int test;
                    read(tube_ACK[i-1][0], &test, sizeof(int)); // Attente de la réponse du transporteur

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

                            // On ne dort pas si c'était la dernière chance, optimisation
                            if (timeout_count < 5) {
                                sleep(rand() % 3 + 1); 
                            }                           
                            break;
                        
                    }
                    
                    }
                    if (succes) {
                        msg_envoyes++;
                        printf("[CLIENT %d] Succès message %d\n", i, j);
                        sleep(rand() % 6); // Attente entre 0 et 5 secondes
                    }
                    else {
                        msg_perdus++;
                        // Si on est ici, c'est que timeout_count vaut 5 et succes vaut 0.
                        printf("----> [CLIENT %d] Abandon après 5 tentatives infructueuses. TERMINAISON.\n", i);
                        
                        // Bilan intermédiaire car le client s'arrête prématurément
                        printf("[BILAN FINAL CLIENT %d - ÉCHEC]\n", i);
                        printf("  - Messages réussis : %d/10\n", msg_envoyes);
                        printf("  - Messages abandonnés : %d/10\n", msg_perdus);

                        close(tube_ACK[i-1][0]); // On ferme proprement
                        exit(0); // LE CLIENT SE TERMINE ICI COMME DEMANDÉ
                    }
            }

            
            close(tube_ACK[i-1][0]); // fermeture lecture après avoir reçu toutes les confirmations
            printf("[BILAN FINAL CLIENT %d - SUCCÈS]\n", i);
            printf("  - Messages réussis : %d/10\n", msg_envoyes);
            printf("  - Messages abandonnés : %d/10\n", msg_perdus);
            exit(0); //fermeture du client quand il a terminé d'envoyer les 10 messages 
        }
    }
    
    close(tube_client_transport[1]); //le transporteur ferme l'ecriture car il n'en aura pas besoin apres boucle pour ne pas casser le tube 
    trame T2; //creation de la zone ou seront récupéré les trames pour controle 
    int nb_valide = 0;


    /*--------------------------TRANSPORT--------------------------*/
    while (read(tube_client_transport[0], &T2, sizeof(trame)) > 0){ // Boucle tant que j'ai des messages 

        printf("[TRANSPORTEUR] Reception du message avec numéro de ports %d et numéro de message %d \n",T2.ports,T2.Nmsg );


        // 1. CONTRÔLE D'INTÉGRITÉ
        // Le transporteur recalcule le checksum sur les données reçues pour vérifier
        // qu'elles n'ont pas été altérées pendant le voyage dans le tube.
        unsigned short checksumCalcule = calculerChecksum(T2.paquet.donnees, 2000, T2.ports, T2.Nmsg);

        int val_Reponse; // Variable stockant le code ACK (0: OK, 1: Err Checksum, 2: Port fermé)
        
        if(checksumCalcule == T2.paquet.checksum){ //controle du checksum + je force le passage sur 16 bits car ~ sur 32 bits fausserai le resultat 
             //pour envoyer le resultat 
            printf("[TRANSPORTEUR] Le checksum de %d numéro %d est juste\n",T2.ports,T2.Nmsg);

            // 2. ROUTAGE (Consultation de l'annuaire en mémoire partagée)
            // On vérifie si un serveur s'est enregistré sur le port demandé par le client
            if (tabPorts[T2.ports-1] != -1) {
                // PORT VALIDE : Un serveur est bien à l'écoute sur ce port
                val_Reponse = 0;
                // ATTENTE ACTIVE : Le transporteur attend que le serveur ait lu le message précédent (bloquant)
                while (tabMsg[T2.ports-1].etat == PLEIN) ;

                printf("[TRANSPORTEUR] Port %d OK -> Transmission au serveur.\n", T2.ports);

                
                // On prépare le paquet pour le serveur
                // Le serveur ne connaît ni le port ni Nmsg, on recalcule donc le checksum avec des 0
                T2.paquet.checksum = calculerChecksum(T2.paquet.donnees, 2000, 0, 0); 

                // 3. TRANSFERT EN MÉMOIRE PARTAGÉE
                // On copie la structure contenant les données utiles dans la boîte du serveur
                tabMsg[T2.ports-1].message = T2.paquet;           
                // On indique au serveur que le message est prêt à être lu
                tabMsg[T2.ports-1].etat = PLEIN;
                nb_valide++;
            } else {
                // PORT FERME (pas de serveur en ecoute)
                // Personne n'a inscrit son ID dans tabPorts pour ce port
                val_Reponse = 2; 
                printf("[TRANSPORTEUR] Port %d FERMÉ -> Information client.\n", T2.ports);
            }
            }
        else{
            // CHECKSUM INVALIDE : Les données ont été corrompues
            val_Reponse = 1; 
            printf("erreur sur le checksum de %d numéro %d calculé %d recu %d\n",T2.ports, T2.Nmsg, checksumCalcule, T2.paquet.checksum);
        }

        // FINALE : On envoie la réponse au client (1 seule ligne pour gérer tous les cas)
        write(tube_ACK[T2.ports-1][1], &val_Reponse, sizeof(int)); 

        //gestion d'attente entre chaque lecture 
        sleep(rand()% 3 + 1); //attend 1 à 3 secondes 
    }

    for (int k = 0; k < N; k++) {
        while (tabMsg[k].etat == PLEIN); // On attend que le dernier message soit lu
        tabMsg[k].etat = 2; // On utilise '2' comme flag
    }

    /*--------------------------NETTOYAGE FINAL--------------------------*/
        
    for(int k = 0; k < N; k++) {
        close(tube_ACK[k][1]); //fermeture de tout les descripteur de tube_ACK
    }
    
    close(tube_client_transport[0]); //fermeture de la lecture de tube 
                    
    while(wait(NULL) > 0); // Attente de tous les fils

    // Destruction propre de la mémoire partagée
    shmdt(tabPorts);
    shmdt(tabMsg);
    if ((shmctl(shmidTabPort, IPC_RMID, NULL) == -1) || (shmctl(shmidTabMsg, IPC_RMID, NULL) == -1)) {
        perror("shmctl");
    } else {
        printf("Nettoyage mémoire partagée OK. Fin du programme.\n");
    }
}