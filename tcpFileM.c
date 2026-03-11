#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
typedef struct paquetServeur {
	unsigned int donnees[2000]; 
	unsigned short checksum; 
} paquetServeur;

typedef struct { //definition de la structure necessaire pour l'utilisation de la file de message
    long type;
    paquetServeur paquet;
} msgFile;


typedef struct Trame{ //creation de la trame 
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
		fprintf(stderr, "[ERREUR] : La commande requiert 2 arguments : %s <nb clients> <nb_server>\n", argv[0]);
		exit(1); 
	}
	int i = 1; //initialisation des clients (numéro de ports)
	int j = 1;  //initialisation du nombre de message par clients 
	int N = atoi(argv[1]); //creation de la variable de N processus 
	int M = atoi(argv[2]); //creation de la variable de M server 

	key_t key = ftok("/tmp", 30);
 	// Création de la zone mémoire de taille N codé sur 2 octets (sizeof(short)) avec les droits de lecture ecriture USER (O600)
	int shmid = shmget(key, N*(sizeof(short)), IPC_CREAT | 0600);
	if (shmid == -1 ) {
		perror("ERREUR shmget");
		exit(1);
	}
	
	short* tabPorts = shmat(shmid, NULL, 0); //on attache la mémoire partagé 
	if (tabPorts == (void *) -1 ) { // shmat renvoie un pointeur void*
		perror("ERREUR shmat"); //controle d'erreur 
		exit(1);
	}

	for (int i = 0; i < N; i++) {
		tabPorts[i] = -1; // Initialisation de touts les elements du tableau a -1 <=> ports libre
	}

	int fileVersServ[N]; //creation des files de messages le fonctionnement de la creation et similaire au tube, N file pour N servers 
	for (int port = 0; port < N; port++) {
  		key_t k = ftok("/tmp", port + 1); // clé unique par port
    		fileVersServ[port] = msgget(k, IPC_CREAT | 0666); //creation + droit
	}

	pid_t pidClient[N]; //création du tableau de PID de tous les clients
	int tube2[N][2]; //creation des tubes retour transport -> client 
	for(int o = 0 ; o < N ; o++){
		pipe(tube2[o]); //creation de touts les tubes TRANSPORT -> CLIENT 
	}
	int tube[2]; //CREATION DU TUBE CLIENT -> TRANSPORT
	
	
	if(pipe(tube) == -1){
    		perror("pipe"); //gestion d'erreur 
   		 exit(0);
	}//creation du tube qui reliera les clients au transporteur




	/*--------------------------SERVEURS--------------------------*/
	pid_t pidServ[M]; //tableau de pid des servers
	for (int i = 1; i <= M; i++) {
		
		paquetServeur paquetRecu; 
		pidServ[i-1] = fork(); //creation des servers (fils)
		int randPort;
		int tentative = 0;
		if (pidServ[i-1] == -1) {
			perror("ERREUR fork serveur"); //gestion erreur fork
			exit(1);
		}
		if (pidServ[i-1] == 0) { //action réalisé par les fils 
			close(tube[0]); // Le serveur ne lit pas les clients
            close(tube[1]); //  Le serveur n'écrit pas au transporteur

			int nb_traites = 0;
            int nb_erreurs_checksum = 0;
			
			for(int k=0; k<N; k++) { 
                close(tube2[k][0]); 
                close(tube2[k][1]); 
            }
			
			srand(123456+i); // On s'assure que la seed est unique
			sleep(rand() % 6);//attente aléatoire entre 0 et 5 secondes avant le lancement du server

			randPort = rand() % N + 1; //connection a un port aleatoire au plus N
			while (tentative < N && tabPorts[randPort-1] != -1) { 
                randPort = rand() % N + 1;
                tentative++; //si occupé on en essaye un autre
            }
			if (tentative >= N) {
				exit(1);
			}
			tabPorts[randPort-1] = i; // Ports[randPort - 1] occupé


			printf("[SERVEUR %d] : J'écoute sur le port %d\n", i, randPort);
			
			msgFile msgRecu; //creation de la struct de stockage
			while (msgrcv(fileVersServ[randPort-1], &msgRecu, sizeof(paquetServeur), 1, 0) != -1) { //lecture de toutes les données 
				nb_traites++;

  				paquetRecu = msgRecu.paquet; //stockage de la donnée
				// 1. Lire le checksum qui suit immédiatement les données
				
				// 2. Vérifier et afficher un message
				if (calculerChecksum(paquetRecu.donnees, 2000, 0, 0) == paquetRecu.checksum) { //Si checksum juste
					printf("[SERVEUR %d] Message reçu avec succès sur le port %d !\n", i, randPort);
					printf("[SERVEUR %d] '%u' \n", i, paquetRecu.donnees[0]);
				} else {
					nb_erreurs_checksum++;
					printf("[SERVEUR %d] Erreur de checksum sur le port %d...\n", i, randPort);
				}
				
				// 3. Attendre avant la prochaine lecture
				sleep(rand() % 5);
			}			
			printf("[BILAN FINAL SERVEUR %d - Port %d]\n", i, randPort);
            printf("  - Total messages consommés : %d\n", nb_traites);
            printf("  - Erreurs checksum : %d\n", nb_erreurs_checksum);
			exit(0); //fermeture des fils 
		} //TUER LES ZOMBIES 
	}


	
	/*--------------------------CLIENTS--------------------------*/
	for(i = 1 ; i <= N ; i++){ // boucle de création des fils (processus clients)
		pidClient[i-1] = fork(); //creation des fils avec fork
		if(pidClient[i-1] == -1){//gestion d'erreur de fork
			perror("fork");
			exit(1);
		}
		
		if(pidClient[i-1] == 0){ //operations seulement réalisé par les fils 
			close(tube[0]); //fermeture descripteur lecture
			srand(123456+i); //genetation de la graine d'aléa
			for(int k = 0; k < N; k++){
				if(k != i-1) { 
					close(tube2[k][0]);
					close(tube2[k][1]);
				}
			}
			close(tube2[i-1][1]); // on ferme l'ecriture car on veut seuelement lire dessus 

			int msg_envoyes = 0; // Pour les statistiques
			int msg_perdus = 0;

			for(j=0; j<10; j++){ //créations des 10 messages par server
				trame T;
				T.ports = i;
				T.Nmsg = j;

				int succes = 0; // Pour sortir de la boucle de retransmission
				int timeout_count = 0;


				//boucle pour le placement des données dans la trame 
				for(int k =0; k<2000; k++) {
					T.paquet.donnees[k] = rand();
				}
				T.paquet.checksum = calculerChecksum(T.paquet.donnees, 2000, T.ports, T.Nmsg);

				
				// timeout pour gerer les 5 tentatives des clients
				while(!succes && timeout_count < 5) { 
					printf("[CLIENT %d] Envoi message %d\n", i, j);
					
					write(tube[1], &T, sizeof(trame));

					int test;
					read(tube2[i-1][0], &test, sizeof(int)); // Attente de la réponse du transporteur

					switch(test) {
						case 0:
							succes = 1; // On passe au message suivant
							break;
						case 1:
							printf("[CLIENT %d] Erreur Checksum, re-tentative...\n", i);
							// On ne met pas succes à 1, donc ça boucle et renvoie
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
						// Si on est ici, c'est que timeout_count vaut 5 et succes vaut 0.
						msg_perdus++;
						printf("----> [CLIENT %d] Abandon après 5 tentatives infructueuses. TERMINAISON.\n", i);
						printf("[BILAN FINAL CLIENT %d - ÉCHEC]\n", i);
                        printf("  - Messages réussis : %d/10\n", msg_envoyes);
                        printf("  - Messages abandonnés : %d/10\n", msg_perdus);

						close(tube2[i-1][0]); // On ferme proprement
						exit(0); // LE CLIENT SE TERMINE ICI COMME DEMANDÉ
					}
			}

			
			close(tube2[i-1][0]); // fermeture lecture après avoir reçu toutes les confirmations
			printf("[BILAN FINAL CLIENT %d - SUCCÈS]\n", i);
            printf("  - Messages réussis : %d/10\n", msg_envoyes);
            printf("  - Messages abandonnés : %d/10\n", msg_perdus);
			exit(0); //fermeture du client quand il a terminé d'envoyer les 10 messages 
		}
	}
	
	close(tube[1]); //le transporteur ferme l'ecriture car il n'en aura pas besoin apres boucle pour ne pas casser le tube 
	trame T2; //creation de la zone ou seront récupéré les trames pour controle


	/*--------------------------TRANSPORT--------------------------*/
	while (read(tube[0], &T2, sizeof(trame)) > 0){ // Boucle tant que j'ai des messages 

		printf("[TRANSPORTEUR] Reception du message avec numéro de ports %d et numéro de message %d \n",T2.ports,T2.Nmsg );

		unsigned short checksumCalcule = calculerChecksum(T2.paquet.donnees, 2000, T2.ports, T2.Nmsg);

		int valReponse;
		
		if(checksumCalcule == T2.paquet.checksum){ //controle du checksum + je force le passage sur 16 bits car ~ sur 32 bits fausserai le resultat 
			 //pour envoyer le resultat 
			printf("[TRANSPORTEUR] Le checksum de %d numéro %d est juste\n",T2.ports,T2.Nmsg);

			if (tabPorts[T2.ports-1] != -1) {
				// PORT VALIDE
				valReponse = 0;
				printf("[TRANSPORTEUR] Port %d OK -> Transmission au serveur.\n", T2.ports);
				
				// On prépare le paquet pour le serveur (checksum neutre)
				T2.paquet.checksum = calculerChecksum(T2.paquet.donnees, 2000, 0, 0); // On recalcul sans ports et nmsg pour la verif du serveur
				msgFile msgServ; //on crée le message a envoyé et on definie le type ici 1 pour chaque données
				msgServ.type = 1;
				msgServ.paquet = T2.paquet;
				msgsnd(fileVersServ[T2.ports-1], &msgServ, sizeof(paquetServeur), 0);
				
			} else {
				// PORT FERME (pas de serveur en ecoute)
				valReponse = 2; 
				printf("[TRANSPORTEUR] Port %d FERMÉ -> Information client.\n", T2.ports);
			}
			}
		else{
			// CHECKSUM INVALIDE
			valReponse = 1; //pour envoyer le resultat 
			printf("erreur sur le checksum de %d numéro %d calculé %d recu %d\n",T2.ports, T2.Nmsg, checksumCalcule, T2.paquet.checksum);
		}

		// FINALE : On envoie la réponse au client (1 seule ligne pour gérer tous les cas)
		write(tube2[T2.ports-1][1], &valReponse, sizeof(int)); 

		//gestion d'attente entre chaque lecture 
		sleep(rand()% 3 + 1); //attend 1 à 3 secondes 
	}

	/*--------------------------NETTOYAGE FINAL--------------------------*/
		
	for(int k = 0; k < N; k++) {
		close(tube2[k][1]); //fermeture de tout les descripteur de tube2
		msgctl(fileVersServ[k], IPC_RMID, NULL);//fermeture de toutes les files de message 
	}
	
	close(tube[0]); //fermeture de la lecture de tube 
					
	while(wait(NULL) > 0); // Attente de tous les fils

	// Destruction propre de la mémoire partagée
    shmdt(tabPorts);
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl");
    } else {
        printf("Nettoyage mémoire partagée OK. Fin du programme.\n");
    }
}		
					
