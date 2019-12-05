docker build -t search_engine .
docker run -itd --name search_engine -v $PWD:/workspace -w /workspace search_engine
docker exec -it search_engine curl -OL https://dumps.wikimedia.org/jawiki/latest/jawiki-latest-pages-articles.xml.bz2
