#include <unistd.h>
#include <pthread.h>
#include <SDL.h>

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <utility>

namespace asio = boost::asio;
using boost::asio::ip::tcp;

#define BUFSIZE 2048

#define XSTR(a) #a
#define STR(a) XSTR(a)

uint32_t* pixels;
volatile int client_count = 0;

void set_pixel(uint16_t x, uint16_t y, uint32_t c, uint8_t a)
{
   if(x < PIXEL_WIDTH && y < PIXEL_HEIGHT){
      if(a == 255){ // fast & usual path
         pixels[y * PIXEL_WIDTH + x] = 0xff000000 | c; // ARGB
         return;
      }
      // alpha path
      uint8_t src_r = (c >> 16);
      uint8_t src_g = (c >> 8) & 0xff;
      uint8_t src_b = (c & 0xff);
      uint32_t dst_c = pixels[y * PIXEL_WIDTH + x];
      uint8_t dst_r = (dst_c >> 16);
      uint8_t dst_g = (dst_c >> 8) & 0xff;
      uint8_t dst_b = (dst_c & 0xff);
      uint8_t na = 255 - a;
      uint16_t r = src_r * a + dst_r * na;
      uint16_t g = src_g * a + dst_g * na;
      uint16_t b = src_b * a + dst_b * na;
      pixels[y * PIXEL_WIDTH + x] = 0xff000000 | ((r & 0xff00) << 8) | (g & 0xff00) | (b >> 8); // ARGB
   }
}

void parse_line(const char* buf, size_t size){
    uint32_t x,y,c;

#if 0 // mit alpha, aber ggf. instabil
    if(!strncmp(buf, "PX ", 3)){ // ...frag nicht :D...
        char *pos1 = buf + 3;
        x = strtoul(buf + 3, &pos1, 10);
        if(buf != pos1){
            pos1++;
            char *pos2 = pos1;
            y = strtoul(pos1, &pos2, 10);
            if(pos1 != pos2){
                pos2++;
                pos1 = pos2;
                c = strtoul(pos2, &pos1, 16);
                if(pos2 != pos1){
                    uint8_t a = 255;
                    if((pos1 - pos2) > 6){ // contains alpha
                        a = c & 0xff;
                        c >>= 8;
                    }
                    set_pixel(x,y,c,a);
                }
            }
        }
    }
#else // ohne alpha
    if(sscanf(buf,"PX %u %u %x",&x,&y,&c) == 3){
        set_pixel(x,y,c, 0xff);
    }
#endif
    else if(!strncmp(buf, "SIZE", 4)){
        // static const char out[] = "SIZE " STR(PIXEL_WIDTH) " " STR(PIXEL_HEIGHT) "\n";
        // send(sock, out, sizeof(out), MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    else {
        printf("QUATSCH[%zu]: ", size);
        for (uint32_t j = 0; j < size; j++)
            printf("%c", buf[j]);
        printf("\n");
    }
}

class pixel_client : public std::enable_shared_from_this<pixel_client>
{
public:
    pixel_client(tcp::socket socket)
        : socket_(std::move(socket)) { }

    void start()
    {
        do_read_line();
    }

    void do_read_line()
    {
        /* used to keep reference count to client object */
        auto self(shared_from_this());
        boost::asio::async_read_until(
            socket_, buffer_, '\n',
            [this, self](boost::system::error_code ec, std::size_t /* length */)
            {
                std::istream input(&buffer_);
                std::string line;
                std::getline(input, line, '\n');
                parse_line(line.data(), line.size());

                if (!ec) {
                    do_read_line();
                }
                else {
                    /* client died */
                    --client_count;
                    std::cout << "dead client removed"
                              << " now only " << client_count << " clients"
                              << std::endl;
                }
            });
    }

    tcp::socket socket_;
    boost::asio::streambuf buffer_;
};

//----------------------------------------------------------------------

class pixel_server
{
public:
    pixel_server(asio::io_service& io_service,
                const tcp::endpoint& endpoint)
        : acceptor_(io_service),
          socket_(io_service)
    {
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(
            asio::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();

        do_accept();
    }

private:
    void do_accept()
    {
        acceptor_.async_accept(
            socket_, [this](boost::system::error_code ec)
            {
                if (!ec) {
                    ++client_count;
                    std::cout << "new client "
                              << socket_.remote_endpoint().address().to_string()
                              << " currently " << client_count << " clients"
                              << std::endl;
                    std::make_shared<pixel_client>(std::move(socket_))->start();
                }

                do_accept();
            });
    }

    tcp::acceptor acceptor_;
    tcp::socket socket_;
};

asio::io_service io_service;

void * handle_clients(void * foobar)
{
    tcp::endpoint endpoint(tcp::v4(), PORT);
    pixel_server server(io_service, endpoint);

    io_service.run();
    return 0;
}

int main(){
   SDL_Init(SDL_INIT_VIDEO);
   SDL_ShowCursor(0);

   SDL_Window* window = SDL_CreateWindow(
      "pixel", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      PIXEL_WIDTH, PIXEL_HEIGHT, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
   SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
   SDL_RenderClear(renderer);
   
   SDL_Texture* sdlTexture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      PIXEL_WIDTH, PIXEL_HEIGHT);
   if(!sdlTexture){
      printf("could not create texture");
      SDL_Quit();
      return 1;
   }
   
   pixels = (uint32_t*)calloc(PIXEL_WIDTH * PIXEL_HEIGHT * 4, 1);

   pthread_t thread_id;
   if(pthread_create(&thread_id , NULL, handle_clients , NULL) < 0){
      perror("could not create thread");
      free(pixels);
      SDL_Quit();
      return 1;
   }
   
   while(42){
      SDL_UpdateTexture(sdlTexture, NULL, pixels, PIXEL_WIDTH * sizeof(uint32_t));
      SDL_RenderCopy(renderer, sdlTexture, NULL, NULL);
      SDL_RenderPresent(renderer);
      SDL_Event event;
      if(SDL_PollEvent(&event)){
         if(event.type == SDL_QUIT){
            break;
         }
         if(event.type == SDL_KEYDOWN){
            if(event.key.keysym.sym == SDLK_q){
               break;
            }
            if(event.key.keysym.sym == SDLK_f){
               uint32_t flags = SDL_GetWindowFlags(window);
               SDL_SetWindowFullscreen(window,
                  (flags & SDL_WINDOW_FULLSCREEN) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
               printf("Toggled Fullscreen\n");
            }
         }
      }
   }

   printf("Shutting Down...\n");
   SDL_DestroyWindow(window);
   io_service.stop();
   pthread_join(thread_id, NULL);
   free(pixels);
   SDL_Quit();
   return 0;
}
