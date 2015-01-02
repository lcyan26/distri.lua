#include "ut_socket.h"
#include "uthread/uthread.h"
#include "kn_list.h"


static engine_t g_engine = NULL;

enum{
	UT_CONN,
	UT_SOCK,
};

typedef struct{
	refobj refobj;
	uint8_t type;
	union{
		stream_conn_t conn;
		handle_t            sock;
	};
	kn_list pending_accept;
	kn_list ut_block;
	kn_list packets;
	int err;
	int close_step;
}ut_socket;

struct st_node_accept{
	kn_list_node node;
	handle_t        sock;
};

struct st_node_block{
	kn_list_node node;
	uthread_t      _uthread;
};

int ut_socket_init(engine_t e){
	if(g_engine || !e) return -1;
	g_engine = e;
	return 0;
}

static void ut_socket_destroy(void *ptr){
	printf("ut_socket_destroy\n");
	ut_socket *ut_sock = (ut_socket*)ptr;
	if(ut_sock->type == UT_SOCK){
		kn_close_sock(ut_sock->sock);
	}
	free(ptr);			
}

ut_socket* ut_socket_new1(handle_t sock){
	ut_socket* ut_sock = calloc(1,sizeof(*ut_sock));
	ut_sock->type = UT_SOCK;
	ut_sock->sock = sock;
	kn_list_init(&ut_sock->pending_accept);
	kn_list_init(&ut_sock->ut_block);
	kn_list_init(&ut_sock->packets);
	refobj_init((refobj*)ut_sock,ut_socket_destroy);
	return ut_sock;
}

ut_socket* ut_socket_new2(stream_conn_t conn){
	ut_socket* ut_sock = calloc(1,sizeof(*ut_sock));
	ut_sock->type = UT_CONN;
	ut_sock->conn = conn;
	stream_conn_setud(conn,ut_sock,NULL);
	refobj_init((refobj*)ut_sock,ut_socket_destroy);	
	return ut_sock;
}

static void on_accept(handle_t s,void *ud){
	printf("on_accept\n");
	ut_socket* _ut_sock = (ut_socket*)ud;
	struct st_node_accept *node = calloc(1,sizeof(*node));
	node->sock = s;
	kn_list_pushback(&_ut_sock->pending_accept,(kn_list_node*)node);
	//if there is uthread block on accept,wake it up;
	struct st_node_block *_block = (struct st_node_block*)kn_list_pop(&_ut_sock->ut_block);
	if(_block){
		ut_wakeup(_block->_uthread);
	}else{
		printf("no block\n");
	}
}

ut_socket_t ut_socket_listen(kn_sockaddr *local){
	if(!g_engine) return empty_ident;
	handle_t sock = kn_new_sock(AF_INET,SOCK_STREAM,IPPROTO_TCP);
	if(!sock) return empty_ident;
	ut_socket* _ut_sock = ut_socket_new1(sock);
	kn_sock_listen(g_engine,sock,local,on_accept,(void*)_ut_sock);
	return make_ident((refobj*)_ut_sock);
}

static void  on_packet(stream_conn_t c,packet_t p){
	packet_t rpk = clone_packet(p);
	ut_socket* utsock = (ut_socket*)stream_conn_getud(c);
	kn_list_pushback(&utsock->packets,(kn_list_node*)rpk);
	struct st_node_block *_block = (struct st_node_block*)kn_list_pop(&utsock->ut_block);
	if(_block){
		//if there is uthread block on recv,wake it up
		ut_wakeup(_block->_uthread);
	}
}

static void on_disconnected(stream_conn_t c,int err){
	ut_socket* utsock = (ut_socket*)stream_conn_getud(c);
	utsock->err = err;
	utsock->close_step = 2;
	struct st_node_block *_block;
	while((_block = (struct st_node_block*)kn_list_pop(&utsock->ut_block))){
		ut_wakeup(_block->_uthread);	
	}
	refobj_dec((refobj*)utsock);
}

ut_socket_t ut_accept(ut_socket_t _,uint32_t buffersize,decoder *_decoder){
	ut_socket* ut_sock = (ut_socket*)cast2refobj(_);
	if(!ut_sock) return empty_ident;	
	uthread_t  ut_current = ut_getcurrent();
	if(is_empty_ident(ut_current)){
		if(_decoder){
			destroy_decoder(_decoder);
		}
		refobj_dec((refobj*)ut_sock);
		return empty_ident;
	}
	ut_socket_t _utsock = empty_ident;
	while(1){
		struct st_node_accept *_node = (struct st_node_accept*)kn_list_pop(&ut_sock->pending_accept);
		if(_node){			
			stream_conn_t conn = new_stream_conn(_node->sock,buffersize,_decoder);
			_utsock = make_ident((refobj*)ut_socket_new2(conn));
			stream_conn_associate(g_engine,conn,on_packet,on_disconnected);
			free(_node);
			break;
		}
		if(ut_sock->close_step){
			refobj_dec((refobj*)ut_sock);
			return _utsock;
		}
		struct st_node_block  st;
		memset(&st,0,sizeof(st));
		st._uthread = ut_current;
		kn_list_pushback(&ut_sock->ut_block,(kn_list_node*)&st);
		ut_block(0);
	}
	refobj_dec((refobj*)ut_sock);
	return _utsock;
}

struct st_connect{
	int err;
	handle_t s;
	uthread_t *ut;	
};

void on_connect(handle_t s,int err,void *ud,kn_sockaddr *_)
{
	struct st_connect *_st_connect = (struct st_connect*)ud;
	_st_connect->err = err;
	_st_connect->s = s;
	if(_st_connect->ut){
		if(0 != ut_wakeup(*_st_connect->ut))
			if(s) kn_close_sock(s);
	}	 	
}

ut_socket_t ut_connect(kn_sockaddr *remote,uint32_t buffersize,decoder* _decoder){
	uthread_t  ut_current = ut_getcurrent();
	if(!remote || is_empty_ident(ut_current)){
		if(_decoder){
			destroy_decoder(_decoder);
		}
		return empty_ident;
	}
	handle_t c = kn_new_sock(AF_INET,SOCK_STREAM,IPPROTO_TCP);
	struct st_connect _st_connect = {.err=0,.s=NULL,.ut=NULL};
	int ret = kn_sock_connect(g_engine,c,remote,NULL,on_connect,&_st_connect);
	if(ret < 0){
		if(_decoder)
			destroy_decoder(_decoder);	
		return empty_ident;
	}else if(ret == 0){
		_st_connect.ut = &ut_current;		
		ut_block(0);
	}

	if(_st_connect.s){
		stream_conn_t conn = new_stream_conn(_st_connect.s,buffersize,_decoder);
		ut_socket_t _utsock = make_ident((refobj*)ut_socket_new2(conn));		
		stream_conn_associate(g_engine,conn,on_packet,on_disconnected);		
		return _utsock;
	}else{
		if(_decoder)
			destroy_decoder(_decoder);			
		return empty_ident;
	}
}

int ut_recv(ut_socket_t _,packet_t *p,int *err){
	if(err) *err = 0;
	*p = NULL;
	ut_socket*  _utsock = (ut_socket*)cast2refobj(_);
	if(!_utsock) return -1;
	uthread_t  ut_current = ut_getcurrent();
	if(_utsock->type != UT_CONN || !p || is_empty_ident(ut_current)){
		refobj_dec((refobj*)_utsock);
		return -1;
	}
	do{
		*p = (packet_t)kn_list_pop(&_utsock->packets);
		if(*p){ 
			refobj_dec((refobj*)_utsock);
			return 0;
		}else{
			if(_utsock->close_step == 2){
				if(err) *err = _utsock->err;
				refobj_dec((refobj*)_utsock);				
				return -1;
			}
			struct st_node_block  st;
			memset(&st,0,sizeof(st));
			st._uthread = ut_current;
			kn_list_pushback(&_utsock->ut_block,(kn_list_node*)&st);
			ut_block(0);
		}
	}while(1);
	refobj_dec((refobj*)_utsock);
	return -1;
}

int ut_send(ut_socket_t _,packet_t p){
	ut_socket*  utsock = (ut_socket*)cast2refobj(_);
	if(!utsock){
		return -1;
	}	
	if(utsock->close_step || utsock->type != UT_CONN){
		refobj_dec((refobj*)utsock);
		destroy_packet(p);
		return -1;
	}
	int ret = stream_conn_send(utsock->conn,p);
	refobj_dec((refobj*)utsock);
	return ret;
}

int ut_close(ut_socket_t _){
	ut_socket*  utsock = (ut_socket*)cast2refobj(_);
	if(!utsock){
		return -1;
	}	
	if(utsock->close_step){
		refobj_dec((refobj*)utsock);
		return -1;
	}
	utsock->close_step = 1;
	if(utsock->type == UT_CONN){
		stream_conn_close(utsock->conn);
	}else{
		refobj_dec((refobj*)utsock);
		struct st_node_block *_block;
		while((_block = (struct st_node_block*)kn_list_pop(&utsock->ut_block))){
			ut_wakeup(_block->_uthread);	
		}
	}
	refobj_dec((refobj*)utsock);
	return 0;
}

void ut_socket_run(){
	while(1){
		uschedule();
		if(activecount > 0)
			kn_engine_runonce(p,0);
		else
			kn_engine_runonce(p,1);
	}
}