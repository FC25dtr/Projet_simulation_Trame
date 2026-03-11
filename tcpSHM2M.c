#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <string.h>

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

#define PLEIN 1
#define VIDE 0

typedef struct paquetServeur {
    unsigned int donnees[2000000];
    unsigned short checksum;
} paquetServeur;

typedef struct SHMTAB {
    paquetServeur message;
    int etat;
} shmTab;


unsigned short calcul_checksum(unsigned int *donnees, int size){
    unsigned int sum = 0;
    for(int k = 0; k < size; k++){
        sum += (donnees[k] & 0xFFFF);
        sum += (donnees[k] >> 16) & 0xFFFF;
    }
    while(sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (unsigned short)(~sum & 0xFFFF);
}

struct Trame{
	unsigned short ports;
	unsigned short Nmsg;
	unsigned int *donnees;
	unsigned short checksum;
};
typedef struct Trame trame;
#define BLOCK 4096

int main(int argc, char* argv[]){
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <nb_clients> <nb_servers>\n", argv[0]);
        exit(1);
    }

	union semun arg;
	arg.val = 1;
	key_t key = ftok("/tmp",123);
	
	if(key == -1){
		perror("ftok");
		exit(0);
	}
	
	int semid = semget(key, 1,  IPC_CREAT | 0644);
	
	if(semid == -1){
		perror("semget");
		exit(0);
	}

	semctl(semid, 0, SETVAL, arg);
	
	struct sembuf *sops = (struct sembuf*)malloc(sizeof(struct sembuf));
	sops->sem_num = 0;
    sops->sem_op = -1;
    sops->sem_flg = 0;
	
	int i = 1;
	int j = 1;
	int N = atoi(argv[1]);
	int M = atoi(argv[2]);

	pid_t pidClient[N];
    
	int tube2[N][2];
	for(int o = 0 ; o < N ; o++){
		pipe(tube2[o]);
	}
	int tube[2];
	
	if(pipe(tube) == -1){
    		perror("pipe");
   		exit(0);
	}

	// Suppression des segments résiduels éventuels
	int old_shmid = shmget(key, 1, 0600);
	if (old_shmid != -1) shmctl(old_shmid, IPC_RMID, NULL);

	int shmid = shmget(key, N*(sizeof(short)), IPC_CREAT | 0600);
	if (shmid == -1 ) {
		perror("ERREUR shmget");
		exit(1);
	}

	short* tabPorts = shmat(shmid, NULL, 0);
	if (tabPorts == (void *) -1 ) {
		perror("ERREUR shmat");
		exit(1);
	}
	
	for (int i = 0; i < N; i++) {
		tabPorts[i] = -1;
	}
	
    key_t keyMsg = ftok("/tmp", 40);
    if (keyMsg == -1) {
        perror("ftok keyMsg");
        exit(1);
    }

    // Suppression du segment résiduel éventuel
    int old_shmidTabMsg = shmget(keyMsg, 1, 0600);
    if (old_shmidTabMsg != -1) shmctl(old_shmidTabMsg, IPC_RMID, NULL);

    int shmidTabMsg = shmget(keyMsg, (size_t)N * sizeof(shmTab), IPC_CREAT | 0600);
    if (shmidTabMsg == -1 ) {
        perror("ERREUR shmget tabMsg");
        exit(1);
    }
    shmTab* tabMsg = shmat(shmidTabMsg, NULL, 0);
    if (tabMsg == (void *) -1 ) {
        perror("ERREUR shmat tabMsg");  
        exit(1);
    }
    for (int k = 0; k < N; k++) {
        tabMsg[k].etat = VIDE;
    }

/*<============================================SERVEURS==========================================================>*/
	pid_t pidServ[M];

    for (int i = 1; i <= M; i++) {
        pidServ[i-1] = fork();

        if (pidServ[i-1] == -1) {
            perror("ERREUR fork serveur"); 
            exit(1);
        }

        if (pidServ[i-1] == 0) { 
			int randPort;
			int tentative = 0;
			int nb_traites = 0;
            int nb_erreurs_checksum = 0;

            close(tube[0]); 
            close(tube[1]); 
            for(int k=0; k<N; k++) { 
                close(tube2[k][0]); 
                close(tube2[k][1]); 
            }
            
            srand(123456+i); 
            sleep(rand() % 6);

            randPort = rand() % N + 1; 
            while (tentative < N && tabPorts[randPort-1] != -1) { 
                randPort = rand() % N + 1;
                tentative++;
            }
			if (tentative >= N) {
				exit(1);
			}
            tabPorts[randPort-1] = i; 
            
            printf("[SERVEUR %d] : J'écoute sur le port %d\n", i, randPort);
            
            while (tabMsg[randPort-1].etat != 2) {
                
                if (tabMsg[randPort-1].etat == PLEIN) {
                    nb_traites++;
					if (calcul_checksum(tabMsg[randPort-1].message.donnees, 2000000) == tabMsg[randPort-1].message.checksum) {
                        printf("[SERVEUR %d] Message de 8Mo reçu intact sur port %d ! Data[0] = %u\n", i, randPort, tabMsg[randPort-1].message.donnees[0]);
                    } 
                    else {
						nb_erreurs_checksum++;
						printf("[SERVEUR %d] Erreur de checksum sur le port %d...\n", i, randPort);
					}
                    tabMsg[randPort-1].etat = VIDE;
                    sleep(rand() % 5); 
                } else {
                    usleep(100000);
                }
            }

			printf("[BILAN FINAL SERVEUR %d - Port %d]\n", i, randPort);
            printf("  - Total messages consommés : %d\n", nb_traites);
            printf("  - Erreurs checksum : %d\n", nb_erreurs_checksum);
            exit(0);
        }
    }
	
	
for(i = 1 ; i <= N ; i++){
		pidClient[i-1] = fork();
		if(pidClient[i-1] == -1){
			perror("fork");
			exit(0);
		}

/*<============================================CLIENT==========================================================>*/

		if(pidClient[i-1] == 0){
			close(tube[0]);
			srand(123456+i);
			for(int k = 0; k < N; k++){
   				 if(k != i-1) {
     					close(tube2[k][0]);
        				close(tube2[k][1]);
   				 	}
			}
			close(tube2[i-1][1]);

			int msg_envoyes = 0;
			int msg_perdus = 0;
			
			for(j=0; j<10; j++){
				trame T;
				T.donnees = (unsigned int*)malloc(sizeof(unsigned int)*2000000);
				if (!T.donnees) {
					perror("malloc");
  					exit(0);
				}

				for (int k = 0; k < 2000000; k++) T.donnees[k] = rand();
				
				T.ports = i;
				T.Nmsg = j;
				T.checksum = calcul_checksum(T.donnees, 2000000);

				int succes = 0;
				int timeout_count = 0;

				while (!succes && timeout_count < 5){				
					sops->sem_op = -1;
					semop(semid, sops, 1);
					printf("[CLIENT %d] Je prend la possession du tube pour envoyer ma trame\n",T.ports);
					printf("[CLIENT %d] Envoi du message numéro %d par le port %d\n", i , j, T.ports);
					write(tube[1], &T.ports, sizeof(T.ports));
					write(tube[1], &T.Nmsg, sizeof(T.Nmsg));
					for (int k = 0; k < 2000000; k += BLOCK) {
						int chunk = BLOCK;
						if (k + BLOCK > 2000000){
							chunk = 2000000 - k;
						}
						size_t size = chunk * sizeof(unsigned int);
						int total = 0;
						char *ptr = (char*)&T.donnees[k];

						while (total < (int)size) { 
						int n = write(tube[1], ptr + total, size - total);
						if (n <= 0) {
							perror("write");
								exit(1);
						}
						total += n;
						}
					}
					write(tube[1], &T.checksum, sizeof(T.checksum));
				
					sops->sem_op = 1;
					semop(semid, sops, 1);

					int test = 0; 
                    read(tube2[i-1][0], &test, sizeof(int));
					
					switch(test) {
						case 0:
							succes = 1;
							break;
						case 1:
							printf("[CLIENT %d] Erreur Checksum, re-tentative...\n", i);
							break;
						case 2:
							timeout_count++;
							printf("[CLIENT %d] Port fermé (Essai %d/5), attente...\n", i, timeout_count);
							if (timeout_count < 5) {
                                sleep(rand() % 3 + 1); 
                            }							
							break;
						
					}
				}
				free(T.donnees);
				if (succes) {
					msg_envoyes++;
					printf("[CLIENT %d] Succès message %d\n", i, j);
					sleep(rand() % 6);
				} else {
					msg_perdus++;
					printf("----> [CLIENT %d] Abandon après 5 tentatives infructueuses. TERMINAISON.\n", i);
					printf("[BILAN FINAL CLIENT %d - ÉCHEC]\n", i);
                    printf("  - Messages réussis : %d/10\n", msg_envoyes);
                    printf("  - Messages abandonnés : %d/10\n", msg_perdus);	
									
					close(tube2[i-1][0]);
					exit(0);
				}
			}
			close(tube2[i-1][0]);

			printf("[BILAN FINAL CLIENT %d - SUCCÈS]\n", i);
            printf("  - Messages réussis : %d/10\n", msg_envoyes);
            printf("  - Messages abandonnés : %d/10\n", msg_perdus);
			
			exit(0);
	
					
	}
	}

	close(tube[1]);
	
	
	
/*<===========================================TRANSPORTEUR==================================================>*/	


	trame T2;
	T2.donnees = (unsigned int*)malloc(sizeof(unsigned int)*2000000);
	while (read(tube[0], &T2.ports, sizeof(unsigned short)) > 0) {

        read(tube[0], &T2.Nmsg, sizeof(unsigned short));
        printf("[TRANSPORTEUR]reception du message avec numéro de ports %d et numéro de message %d \n",T2.ports,T2.Nmsg );
		
		for (int k = 0; k < 2000000; k += BLOCK) { 
			int chunk = BLOCK;
            if (k + BLOCK > 2000000){
                chunk = 2000000 - k;
            }
            size_t size = chunk * sizeof(unsigned int);
            int total = 0;
            char *ptr = (char*)&T2.donnees[k];

            while (total < (int)size) { 
                int n = read(tube[0], ptr + total, size - total);
                if (n <= 0) {
                    perror("read client->transport");
                    exit(1);
                }
                total += n;
            }
        }
		
		read(tube[0], &T2.checksum, sizeof(T2.checksum));

        int valReponse = 0;

		if(calcul_checksum(T2.donnees, 2000000) == T2.checksum){
			printf("[TRANPORTEUR]le checksum de %d numéro %d est juste\n",T2.ports,T2.Nmsg);
			
			if (tabPorts[T2.ports-1] != -1) {
				printf("[TRANSPORTEUR] Port %d OK -> Transfert des 8Mo au serveur.\n", T2.ports);

				while (tabMsg[T2.ports-1].etat == PLEIN);

                memcpy(tabMsg[T2.ports-1].message.donnees, T2.donnees, 2000000 * sizeof(unsigned int));
                tabMsg[T2.ports-1].message.checksum = calcul_checksum(T2.donnees, 2000000);
                tabMsg[T2.ports-1].etat = PLEIN;

			} else {
				valReponse = 2;
				printf("[TRANSPORTEUR] Port %d FERMÉ -> Information client.\n", T2.ports);
			}
		}
		else {
			valReponse = 1;
			printf("[TRANSPORTEUR]erreur sur le checksum port=%d msg=%d\n", T2.ports, T2.Nmsg);
		}
		write(tube2[T2.ports-1][1], &valReponse, sizeof(int)); 
		sleep(rand() % 3 + 1);
	}

    for (int k = 0; k < N; k++) {
        while (tabMsg[k].etat == PLEIN);
        tabMsg[k].etat = 2;
    }

    /*<===============================NETTOYAGE================================================>*/
    
	for(int k = 0; k < N; k++){
       		close(tube2[k][1]);
        }

	close(tube[0]);
				
	while(wait(NULL) > 0);

    semctl(semid, 0, IPC_RMID, arg);
	free(sops);
	free(T2.donnees);

	shmdt(tabPorts);
    shmdt(tabMsg);
    if ((shmctl(shmid, IPC_RMID, NULL) == -1) || (shmctl(shmidTabMsg, IPC_RMID, NULL) == -1)) {
        perror("shmctl");
    } else {
        printf("Nettoyage mémoire partagée OK. Fin du programme.\n");
    }

    printf("\n[SYSTEME] Nettoyage terminé tout est OK\n\n");
}

