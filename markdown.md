savoir loc des autres
savoir réseau wifi

camera selon lock

augmenter orbustesse changmeent ecran/brnahcement ecran poru eviter probleme

corriger bug ou quand on lance avec fichier en dehors cash

augmenter robustesse probleme co

.env avec les varialbes lesp lus importantes : ex : timeout avant arrête de la scene si connexion perdue avec la perosnne qui l'a lancéep our eviter qu'elle reste`
indéfiniment

quando n quitte une scene au bout de 3 minutes sans être dessus, son contenu doit
être déchargé de la ram. ok ?
l'app doit très tèrs peu consommer en stand by. 





ok excellent ! ya justeu n bug très génant. Quand on clique sur l'outil texte, quand on ajotue un texte, i lesty par défaut sélecitonné, on peut le déplacer oduble cliquer dessus poru editer; MAIS si ensutie je l'unselect, et que je clique desuss pour le reselectionner rien dne sep asse.. même chose si je double clique srien ne se passe. Ya un gros soucis. INvesituge, toruve le probleme sturcutrel de fond et corrige le proprement déifntivement, de maniere perenne. Verifie qu'on a pas ce probleme de maniere général avec d'autes éléments. De plus si tu reperes des faiblaisses dans la structure lgobal du système, corrige les au passage. On doit avoir un client extremement bien strucutré, organisé, robuste




Ok, excellent. Maintenant, j'aimerais que tu résolves un autre problème, c'est les vidéos. Alors déjà, pour le format accepté, pour l'instant, j'aimerais qu'on le restreigne uniquement au MP4. Ensuite, pour les vidéos, le souci que j'ai, c'est que quand j'uploade une vidéo sur le canvas, quand j'en mets une sur le canvas, eh bien, quand je la joue, j'ai pas de son du tout. Le son n'arrive pas alors elle se joue. Il y a quelques petites lenteurs, j'ai l'impression, c'est pas dingue et puis surtout le son n'arrive jamais. Alors, donc j'aimerais que tu règles ça, en fait. J'aimerais que tu règles ce bug. Et de manière générale aussi, j'aimerais que tu analyses le composant vidéo et que tu vérifies s'il est correct, s'il est optimisé, si sa structure est normale, s'il s'intègre bien au Canvas QT Quick ou s'il relève de mauvaises pratiques ou de mauvaises fondations qui devraient être repensées pour être beaucoup plus robuste. Je veux également que tu vérifies s'il est parfaitement synchronisé, cohérent par rapport à la version distante de la scène. Tu sais qu'on peut lancer une scène à distance et il faut qu'on ait exactement le replica des mêmes objets avec les mêmes états sur le client distant lorsqu'on lance la scène. Donc là, voilà, vérifier aussi ça et de manière générale, si tu vois le moindre souci de structure, de pratiques par rapport à la vidéo, les régler pour que visuellement, en termes d'UI et de fonctionnalités, je veux rien que tu changes, mais par contre je veux que les choses soient robustes, que la vidéo soit ultra performante dans le canvas, parfaitement répliquée sur le client distant avec toutes les fonctionnalités qui fonctionnent correctement.


Ok, excellent. Maintenant, j'aimerais que tu résolves un autre problème, c'est les vidéos. Alors déjà, pour le format accepté, pour l'instant, j'aimerais qu'on le restreigne uniquement au MP4. Ensuite, pour les vidéos, le souci que j'ai, c'est que quand j'uploade une vidéo sur le canvas, quand j'en mets une sur le canvas, eh bien, quand je la joue, j'ai pas de son du tout. Le son n'arrive pas alors elle se joue. Il y a quelques petites lenteurs, j'ai l'impression, c'est pas dingue et puis surtout le son n'arrive jamais. Alors, donc j'aimerais que tu règles ça, en fait. J'aimerais que tu règles ce bug. Et de manière générale aussi, j'aimerais que tu analyses le composant vidéo et que tu vérifies s'il est correct, s'il est optimisé, si sa structure est normale, s'il s'intègre bien au Canvas QT Quick ou s'il relève de mauvaises pratiques ou de mauvaises fondations qui devraient être repensées pour être beaucoup plus robuste. Je veux également que tu vérifies s'il est parfaitement synchronisé, cohérent par rapport à la version distante de la scène. Tu sais qu'on peut lancer une scène à distance et il faut qu'on ait exactement le replica des mêmes objets avec les mêmes états sur le client distant lorsqu'on lance la scène. Donc là, voilà, vérifier aussi ça et de manière générale, si tu vois le moindre souci de structure, de pratiques par rapport à la vidéo, les régler pour que visuellement, en termes d'UI et de fonctionnalités, je veux rien que tu changes, mais par contre je veux que les choses soient robustes, que la vidéo soit ultra performante dans le canvas, parfaitement répliquée sur le client distant avec toutes les fonctionnalités qui fonctionnent correctement.

Excellent. Il y a un autre souci que j'aimerais relayer qui est assez étonnant. Juste avant, je t'ai demandé d'optimiser la bordure, c'est-à-dire que quand on ajoutait une bordure importante sur un texte, ça créait des gros bugs dans le déplacement du canvas, enfin ça faisait buguer le canvas. Maintenant, c'est incroyablement bien optimisé, mais du coup c'est assez rigolo. Quand j'ai un texte avec une bordure et que je me déplace dans le canvas, c'est incroyablement fluide, il n'y a aucun lag. Par contre, si j'ai un texte sans bordure dans le canvas et que je me déplace, eh ben ça saccade beaucoup, ça lag pas mal au niveau des déplacements. Donc manifestement, quand il y a une bordure, il doit y avoir une sorte d'optimisation de logique de rendu avec le texte qui est parfaite, qui est très bien pensée, qui est très pertinente et qu'on n'a pas avec le texte sans bordure. Donc il faut que t'analyses ça. Il faut que t'analyses quelle est l'optimisation, et si on n'a pas, résoudre tout simplement le problème pour que lorsqu'on a un élément texte dans le canvas, ça ralentisse pas le mouvement de la caméra, ça fasse pas buguer le canvas de manière générale, et aussi peut-être avoir une réflexion, ça serait extrêmement pertinent, sur les autres éléments pour étendre cette logique à l'ensemble des éléments, que ce soit vidéo, texte, image, afin qu'une vidéo, un texte ou une image, ou un texte avec une bordure, ne fasse absolument pas laguer ou saccader le déplacement de la caméra et le canvas.