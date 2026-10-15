#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float f32;
typedef double f64;

#define ANSI_FOREGROUND_RED "\x1b[31m"
#define ANSI_FOREGROUND_DEFAULT "\x1b[39m"

#define MAX_FILE_COUNT            ( 1024 )
#define MAX_FILE_BUFFER_COUNT     ( 1024 * 1024 )
#define MAX_GLOBAL_REGISTER_COUNT ( 1024 * 16 )
#define MAX_TYPE_COUNT            ( 1024 * 16 )
#define MAX_PROCEDURE_COUNT       ( 1024 * 16 )
#define MAX_OUTPUT_COUNT          ( 1024 * 1024 )
#define MAX_SCRATCH_COUNT         ( 1024 * 16 )

#define TOKEN_ERROR 0
#define TOKEN_NUMBER 1
#define TOKEN_PROCEDURE 2
#define TOKEN_ROUTINE 3
#define TOKEN_STRUCTURE 4
#define TOKEN_RETURN 5
#define TOKEN_JUMP 6
#define TOKEN_CONTITIONAL_JUMP 7
#define TOKEN_CALL 8
#define TOKEN_REGISTER 9
#define TOKEN_ARGUMENT 10
#define TOKEN_TYPE 11
#define TOKEN_ARRAY 12
#define TOKEN_ADDRESS 13
#define TOKEN_STORE 14
#define TOKEN_LOAD 15
#define TOKEN_ADD 16

#define TOKEN_IDENTIFIER 128
#define TOKEN_CATEGORY_END 129
#define TOKEN_STATEMENT_END 130
#define TOKEN_SIGNATURE_END 131
#define TOKEN_ASSIGNMENT 132
#define TOKEN_EOF 255

char* token_kind_string[ 17 ] = {
	"error             ",
	"number            ",
	"procedure         ",
	"routine           ",
	"structure         ",
	"return            ",
	"jump              ",
	"contitional_jump  ",
	"call              ",
	"register          ",
	"argument          ",
	"type              ",
	"array             ",
	"address           ",
	"store             ",
	"load              ",
	"add               ",
};

typedef struct {
	char* raw;
	i16 bytes;
	i16 file;
	i16 sibling;
	u8 kind;
	u8 flags;
} syntax_token;


typedef struct {
	char* name;
	char* raw;
	char* parse;
	i32 name_bytes;
	i32 raw_bytes;
} source_file;

char raw_file_buffer[ MAX_FILE_BUFFER_COUNT ];
i32 raw_file_buffer_count = 0;
source_file file_buffer[ MAX_FILE_COUNT ];
i32 file_buffer_count = 0;
pthread_mutex_t file_mutex;
_Atomic i32 files_started_parsing = 0;
_Atomic i32 files_finished_parsing = 0;

syntax_token procedure_buffer[ MAX_PROCEDURE_COUNT ];
i32 procedure_buffer_count = 0;
i32 procedure_count = 0;
pthread_mutex_t procedure_mutex;
_Atomic i32 procedures_started_validating = 0;
_Atomic i32 procedures_finished_validating = 0;
_Atomic i32 procedure_buffer_index_validating = 0;

syntax_token global_register_buffer[ MAX_GLOBAL_REGISTER_COUNT ];
i32 global_register_buffer_count = 0;
pthread_mutex_t global_register_mutex;

syntax_token type_buffer[ MAX_TYPE_COUNT ];
i32 type_buffer_count = 0;
pthread_mutex_t type_mutex;

char output_buffer[ MAX_OUTPUT_COUNT ];
i32 output_buffer_count = 0;
pthread_mutex_t output_mutex;
_Atomic i32 procedures_started_outputing = 0;
_Atomic i32 procedures_finished_outputing = 0;
_Atomic i32 procedure_buffer_index_outputed = 0;

_Thread_local u8 scratch[ MAX_SCRATCH_COUNT ];
_Thread_local i32 scratch_count;

void assert_failed( char* file, i32 line, char* expression ){
	fprintf( stderr, "%s%s:%d%s \"%s\"\n", ANSI_FOREGROUND_RED, file, line, ANSI_FOREGROUND_DEFAULT, expression );
	fflush( stderr );
	exit( 1 );
}

#define assert( expression ){\
	if( !( expression )){\
		assert_failed( __FILE__, __LINE__, #expression );\
	}\
}

void error( char* format, ... ){
	fprintf( stderr, "%sError%s ", ANSI_FOREGROUND_RED, ANSI_FOREGROUND_DEFAULT );
	{
		va_list args;
		va_start( args, format );
		vfprintf( stderr, format, args );
		va_end( args );
	}
	fprintf( stderr, "\n" );
	fflush( stderr );
	exit( 1 );
}

void buffer_append( char* dst, i32* dst_count, char* src, i32 src_count ){
	assert( dst != NULL );
	assert( *dst_count >= 0 );
	assert( src != NULL );
	assert( src_count >= 0 );
	memmove( &dst[ *dst_count ], src, src_count );
	*dst_count += src_count;
}

void buffer_insert( char* dst, i32* dst_count, i32 index, char* src, i32 src_count ){
	assert( dst != NULL );
	assert( *dst_count >= 0 );
	assert( index >= 0 );
	assert( src != NULL );
	assert( src_count >= 0 );
	memmove( &dst[ index + src_count ], &dst[ index ], *dst_count - index );
	memmove( &dst[ index ], src, src_count );
	*dst_count += src_count;
}

void buffer_delete( char* dst, i32* dst_count, i32 index, i32 count ){
	assert( dst != NULL );
	assert( *dst_count >= 0 );
	assert( index >= 0 );
	assert( count >= 0 );
	assert( index + count <= *dst_count );
	memmove( &dst[ index ], &dst[ index + count ], *dst_count - index - count );
	*dst_count -= count;
}

void file_add( char* name, i32 name_bytes ){
	assert( name != NULL );
	assert( name_bytes > 0 );
	assert( name_bytes <= 4096 );
	assert( MAX_FILE_COUNT > file_buffer_count + 1 );
	char* name_null_terminated = __builtin_alloca( name_bytes + 1 );
	memcpy( name_null_terminated, name, name_bytes );
	name_null_terminated[ name_bytes ] = '\0';
	FILE* fp = fopen( name_null_terminated, "r" );
	if( fp == NULL ){
		printf( "%sError%s Failed to open file '%s'.\n", ANSI_FOREGROUND_RED, ANSI_FOREGROUND_DEFAULT, name_null_terminated );
		exit( 1 );
	}
	i32 error = fseek( fp, 0, SEEK_END );
	assert( !error );
	i64 file_length = ftell( fp );
	assert( file_length >= 0 );
	rewind( fp );
/* critical section */
	error = pthread_mutex_lock( &file_mutex );
	assert( !error );
	i64 fread_result = fread( &raw_file_buffer[ raw_file_buffer_count ], 1, file_length, fp );
	assert( fread_result == file_length );
	file_buffer[ file_buffer_count ].raw = &raw_file_buffer[ raw_file_buffer_count ];
	file_buffer[ file_buffer_count ].parse = file_buffer[ file_buffer_count ].raw;
	file_buffer[ file_buffer_count ].raw_bytes = file_length;
	file_buffer[ file_buffer_count ].name = name;
	file_buffer[ file_buffer_count ].name_bytes = name_bytes;
	raw_file_buffer_count += fread_result;
	file_buffer_count += 1;
	error = pthread_mutex_unlock( &file_mutex );
	assert( !error );
/* end critical section */
	fclose( fp );
}

void print_tokens(){
	assert( global_register_buffer_count == 0 );
	assert( type_buffer_count == 0 );
	printf( "\nTypes:\n\nGlobals:\n\nProcedures:\n" );
	i32 i = 0;
	while( i < procedure_buffer_count ){
		i32 indent;
		if( procedure_buffer[ i ].kind == TOKEN_PROCEDURE ){
			indent = 2;
		} else if( procedure_buffer[ i ].kind == TOKEN_ROUTINE ){
			indent = 4;
		} else if( procedure_buffer[ i ].sibling == 0 ){
			indent = 8;
		} else {
			indent = 6;
		}
		printf( "%*c%s%*c%d:%d  [%.*s]\n", indent, ' ', token_kind_string[ procedure_buffer[ i ].kind ], 9 - indent, ' ', 
		        procedure_buffer[ i ].sibling, procedure_buffer[ i ].file, procedure_buffer[ i ].bytes, procedure_buffer[ i ].raw );
		i++;
	}
	printf( "\n" );
}

_Atomic i8 found_error = 0;

void compiler_error( syntax_token token, char* message ){
/* absolute hack to prevent multiple threads trying to report errors at the same time. */
	if( found_error == 0 ) found_error = 1; else sleep( 1 );
	assert( token.bytes > 0 );
	assert( token.raw != NULL );
	source_file* file = &file_buffer[ token.file ];
	char* error_line_start = file->raw;
	i32 token_line = 0;
	i32 i = 0;
	while( i < token.raw - file->raw ){
		if( file->raw[ i ] == '\n' ){
			token_line++;
			error_line_start = file->raw + i + 1;
		}
		i++;
	}
	while( file->raw[ i ] != '\n' ){
		i++;
	}
	i32 pre_error_bytes = token.raw - error_line_start;
	i32 post_error_bytes = &file->raw[ i ] - error_line_start - pre_error_bytes - token.bytes;
	printf( "%sError%s %s\n%.*s:%d: %.*s%s%.*s%s%.*s\n", ANSI_FOREGROUND_RED, ANSI_FOREGROUND_DEFAULT, message, file->name_bytes, file->name, token_line,
	        pre_error_bytes, error_line_start, ANSI_FOREGROUND_RED,
	        token.bytes, error_line_start + pre_error_bytes, ANSI_FOREGROUND_DEFAULT, 
		post_error_bytes, error_line_start + pre_error_bytes + token.bytes );
	print_tokens();
	exit( 1 );
}

syntax_token token_next( source_file* file ){
	assert( file != NULL );
	syntax_token token = { 0 };
	token.file = file - file_buffer;
	char* s_max = file->raw + file->raw_bytes;
	while( 1 ){
		while( file->parse < s_max && ( *file->parse == ' ' || *file->parse == '\n' || *file->parse == '\r' || *file->parse == '\t' )){
			file->parse++;
		}
		if( *file->parse == '{' ){
			i32 depth = 1;
			file->parse++;
			while( file->parse < s_max && depth > 0 ){
				if( *file->parse == '{' ){
					depth += 1;
				} else if( *file->parse == '}' ){
					depth -= 1;
				}
				file->parse++;
			}
			if( file->parse == s_max && depth > 0 ){
				token.raw = file->parse;
				token.bytes = 1;
				compiler_error( token, "Unclosed comment." );
			}
		} else if( *file->parse == '}' ){
			token.raw = file->parse;
			token.bytes = 1;
			compiler_error( token, "Unopened comment." );
		} else {
			break;
		}
	}
	assert( file->parse <= s_max )
	if( file->parse == s_max ){
		token.kind = TOKEN_EOF;
		return token;
	}
	token.raw = file->parse;
	if(( *file->parse >= 'a' && *file->parse <= 'z' ) || ( *file->parse >= 'A' && *file->parse <= 'Z' ) || *file->parse == '_' ){
		do {
			token.bytes++;
			file->parse++;
		} while(( *file->parse >= 'a' && *file->parse <= 'z' ) || ( *file->parse >= 'A' && *file->parse <= 'Z' ) || ( *file->parse >= '0' && *file->parse <= '9' ) || *file->parse == '_' );
		if( token.bytes == 9 && strncmp( token.raw, "procedure", 9 ) == 0 ){
			token.kind = TOKEN_PROCEDURE;
		} else if( token.bytes == 6 && strncmp( token.raw, "return", 6 ) == 0 ){
			token.kind = TOKEN_RETURN;
		} else if( token.bytes == 3 && strncmp( token.raw, "add", 3 ) == 0 ){
			token.kind = TOKEN_ADD;
		} else {
			token.kind = TOKEN_IDENTIFIER;
		}
	} else if( *file->parse >= '0' && *file->parse <= '9' ){
		token.kind = TOKEN_NUMBER;
		do {
			token.bytes++;
			file->parse++;
		} while(( *file->parse >= '0' && *file->parse <= '9' ) || *file->parse == '_' );
	} else if( *file->parse == '|' ){
		token.bytes = 1;
		token.kind = TOKEN_CATEGORY_END;
		file->parse++;
	} else if( *file->parse == ';' ){
		token.bytes = 1;
		token.kind = TOKEN_STATEMENT_END;
		file->parse++;
	} else if( *file->parse == ':' ){
		token.bytes = 1;
		token.kind = TOKEN_SIGNATURE_END;
		file->parse++;
	} else if( *file->parse == '=' ){
		token.bytes = 1;
		token.kind = TOKEN_ASSIGNMENT;
		file->parse++;
	} else if( *file->parse == '!' ){
		token.bytes = 1;
		token.kind = TOKEN_JUMP;
		file->parse++;
	} else {
		token.bytes = 1;
		compiler_error( token, "Invalid chracter" );
	}
	assert( token.bytes > 0 );
	assert( token.raw != NULL );
	return token;
}

void push_procedure( source_file* file ){
	assert( file != NULL );
	assert( scratch_count >= 3 ); /* smallest valid procedure { proc procedure(): !return }. One procedure, one routine, one return */
	assert( ((syntax_token*)scratch)[ 0 ].kind == TOKEN_PROCEDURE );
	assert( procedure_buffer_count >= 0 );
/* critical section */
	i32 error = pthread_mutex_lock( &procedure_mutex );
	assert( !error );
	memcpy( &procedure_buffer[ procedure_buffer_count ], &((syntax_token*)scratch)[ 0 ], sizeof( ((syntax_token*)scratch)[ 0 ]) * scratch_count );
	procedure_buffer_count += scratch_count;
	procedure_count += 1;
	error = pthread_mutex_unlock( &procedure_mutex );
/* end critical section */
	assert( !error );
	scratch_count = 0;
}

syntax_token* push_token( syntax_token token ){
	assert( token.raw != NULL );
	assert( token.bytes > 0 );
	assert(( token.kind & 0x80 ) == 0 );
	((syntax_token*)scratch)[ scratch_count ] = token;
	scratch_count += 1;
	return &((syntax_token*)scratch)[ scratch_count - 1 ];
}

void parse_file( source_file* file ){
	assert( file != NULL );
	syntax_token token = token_next( file );
	syntax_token type_token = token_next( file );
	while( token.kind != TOKEN_EOF ){
		if( token.kind != TOKEN_IDENTIFIER ){
			compiler_error( token, "Expected name of global register / proceudre / type." );
		}
		if( type_token.kind == TOKEN_PROCEDURE ){
			token.kind = TOKEN_PROCEDURE;
			assert( scratch_count == 0 );
			i32 procedure_start = scratch_count;
			syntax_token* procedure_token = push_token( token );
			token = token_next( file );
			if( token.kind != TOKEN_IDENTIFIER ){
				compiler_error( token, "Expected return type." );
			}
			token.kind = TOKEN_TYPE;
			push_token( token );
			token = token_next( file );
			if( token.kind != TOKEN_CATEGORY_END ){
				compiler_error( token, "Expected category end '|'." );
			}
			type_token.kind = TOKEN_ROUTINE;
			i32 routine_start = scratch_count;
			syntax_token* routine_token = push_token( type_token );
			token = token_next( file );
			while( token.kind != TOKEN_SIGNATURE_END ){
				if( token.kind != TOKEN_IDENTIFIER ){
					compiler_error( token, "Expected procedure argument name or signature end." );
				}
				token.kind = TOKEN_ARGUMENT;
				token.sibling = 1;
				push_token( token );
				token = token_next( file );
				if( token.kind != TOKEN_IDENTIFIER ){
					compiler_error( token, "Expected procedure argument type." );
				}
				token.kind = TOKEN_TYPE;
				push_token( token );
				token = token_next( file );
			}
			while( 1 ){
				i32 statement_start = scratch_count;
				token = token_next( file );
				type_token = token_next( file );
				if( token.kind == TOKEN_IDENTIFIER ){
					token.kind = TOKEN_REGISTER;
					syntax_token* statement_token = push_token( token );
					if( type_token.kind != TOKEN_IDENTIFIER ){
						compiler_error( token, "Expected type of register" );
					}
					type_token.kind = TOKEN_TYPE;
					push_token( type_token );
					token = token_next( file );
					if( token.kind != TOKEN_ASSIGNMENT ){
						compiler_error( token, "Expected register assignment." );
					}
					token = token_next( file );
					if( token.kind == TOKEN_NUMBER ){
						push_token( token );
					} else if( token.kind == TOKEN_IDENTIFIER ){
						token.kind = TOKEN_REGISTER;
						push_token( token );
					} else if( token.kind == TOKEN_ADD ){
						push_token( token );
						token = token_next( file );
						if( token.kind == TOKEN_NUMBER ){
							push_token( token );
						} else if( token.kind == TOKEN_IDENTIFIER ){
							token.kind = TOKEN_REGISTER;
							push_token( token );
						} else {
							compiler_error( token, "Unexpected add opperand." );
						}
						token = token_next( file );
						if( token.kind == TOKEN_NUMBER ){
							push_token( token );
						} else if( token.kind == TOKEN_IDENTIFIER ){
							token.kind = TOKEN_REGISTER;
							push_token( token );
						} else {
							compiler_error( token, "Unexpected add opperand." );
						}
					} else {
						compiler_error( token, "Unexpected register value." );
					}
					token = token_next( file );
					if( token.kind != TOKEN_STATEMENT_END ){
						compiler_error( token, "Expected end of statement." );
					}
					statement_token->sibling = scratch_count - statement_start;
				} else if( token.kind == TOKEN_JUMP ){
					if( type_token.kind != TOKEN_RETURN ){
						compiler_error( token, "Expected jump location." );
					}
					syntax_token* statement_token = push_token( type_token );
					token = token_next( file );
					if( token.kind == TOKEN_NUMBER ){
					} else if( token.kind == TOKEN_IDENTIFIER ){
						token.kind = TOKEN_REGISTER;
					} else {
						compiler_error( token, "Expected jump argument." );
					}
					push_token( token );
					token = token_next( file );
					if( token.kind != TOKEN_STATEMENT_END ){
						compiler_error( token, "Expected register value." );
					}
					statement_token->sibling = scratch_count - statement_start;
					routine_token->sibling = scratch_count - routine_start;
					token = token_next( file );
					type_token = token_next( file );
/*TODO: check for next routine */
					procedure_token->sibling = scratch_count - procedure_start;
					break;
				} else {
					compiler_error( type_token, "Expected start of statement." );
				}
			}
			push_procedure( file );
		} else {
			compiler_error( type_token, "Expected type of global register / proceudre / type." );
		}
	}
	files_finished_parsing++;
}

void validate_is_a_type( syntax_token* token ){
	assert( token != NULL );
	assert( token->raw != NULL );
	assert( token->bytes > 0 );
	if( token->bytes == 3 && strncmp( token->raw, "i32", 3 ) == 0 ){
	} else {
		compiler_error( *token, "Must be a type." );
	}
}

void validate_not_a_type( syntax_token* token ){
	assert( token != NULL );
	assert( token->raw != NULL );
	assert( token->bytes > 0 );
	if( token->bytes == 3 && strncmp( token->raw, "i32", 3 ) == 0 ){
		compiler_error( *token, "Must be a type." );
	}
}

void validate_exclusive_global( syntax_token* procedure ){
/* TODO: check for types and global registers */
	assert( procedure != NULL );
	assert( procedure->raw != NULL );
	assert( procedure->bytes > 0 );
	assert( procedure->kind == TOKEN_PROCEDURE );
	if( procedure->bytes == 3 && strncmp( procedure->raw, "i32", 3 ) == 0 ){
		compiler_error( *procedure, "Globals have the same name." );
	}
	syntax_token* i = procedure_buffer;
	while( i < procedure ){
		if( i->bytes == procedure->bytes && strncmp( i->raw, procedure->raw, i->bytes ) == 0 ){
			compiler_error( *procedure, "Globals have the same name." );
		}
		assert( i->sibling > 0 );
		i += i->sibling;
	}
}

void validate_exclusive_routine( syntax_token* procedure, syntax_token* routine ){
	assert( procedure != NULL );
	assert( procedure->raw != NULL );
	assert( procedure->bytes > 0 );
	assert( procedure->kind == TOKEN_PROCEDURE );
	assert( routine != NULL );
	assert( routine->raw != NULL );
	assert( routine->bytes > 0 );
	assert( routine->kind == TOKEN_ROUTINE );
	syntax_token* i = procedure;
	while( i->kind != TOKEN_ROUTINE ){
		i++;
	}
	while( i < routine ){
		if( i->bytes == routine->bytes && strncmp( i->raw, routine->raw, i->bytes ) == 0 ){
			compiler_error( *routine, "Routines have the same name." );
		}
		assert( i->sibling > 0 );
		i += i->sibling;
	}
}

void validate_exclusive_register( syntax_token* procedure, syntax_token* reg ){
/* Will break with naked procedure calls or more than one routine */
	assert( procedure != NULL );
	assert( procedure->raw != NULL );
	assert( procedure->bytes > 0 );
	assert( procedure->kind == TOKEN_PROCEDURE );
	assert( reg != NULL );
	assert( reg->raw != NULL );
	assert( reg->bytes > 0 );
	assert( reg->sibling > 0 );
	assert( reg->kind == TOKEN_REGISTER || reg->kind == TOKEN_ARGUMENT );
	syntax_token* i = procedure;
	i++;
	while( i->kind == TOKEN_TYPE ){
		i++;
	}
	assert( i->kind == TOKEN_ROUTINE );
	i++;
	while( i < reg ){
		assert( i->kind == TOKEN_REGISTER || i->kind == TOKEN_ARGUMENT );
		assert( i->sibling > 0 );
		if( i->bytes == reg->bytes && strncmp( i->raw, reg->raw, i->bytes ) == 0 ){
			compiler_error( *reg, "Registers have the same name." );
		}
		i += i->sibling;
	}
}

void validate_is_a_register( syntax_token* procedure, syntax_token* reg ){
	assert( procedure != NULL );
	assert( procedure->raw != NULL );
	assert( procedure->bytes > 0 );
	assert( procedure->kind == TOKEN_PROCEDURE );
	assert( reg != NULL );
	assert( reg->raw != NULL );
	assert( reg->bytes > 0 );
	assert( reg->sibling == 0 );
	assert( reg->kind == TOKEN_REGISTER );
	syntax_token* statement = reg;
	while( statement->sibling == 0 ){
		statement -= 1;
	}
	assert( statement->kind == TOKEN_RETURN || statement->kind == TOKEN_REGISTER );
	syntax_token* i = procedure;
	i++;
	while( i->kind == TOKEN_TYPE ){
		i++;
	}
	assert( i->kind == TOKEN_ROUTINE );
	i++;
	while( i < statement ){
		assert( i->sibling > 0 );
		if( i->kind == TOKEN_REGISTER || i->kind == TOKEN_ARGUMENT ){
			if( i->bytes == reg->bytes && strncmp( i->raw, reg->raw, i->bytes ) == 0 ){
				return;
			}
			assert( i->sibling > 0 );
			i += i->sibling;
		} else if( i->kind == TOKEN_ROUTINE ){
			assert( 0 );
		} else if( i->kind == TOKEN_RETURN ){
			assert( i->sibling > 0 );
			i += i->sibling;
		} else {
			assert( 0 );
		}
	}
	compiler_error( *reg, "Is not a reg" );
}

syntax_token* validate_value( syntax_token* procedure, syntax_token* value ){
	assert( procedure != NULL );
	assert( procedure->raw != NULL );
	assert( procedure->bytes > 0 );
	assert( procedure->kind == TOKEN_PROCEDURE );
	assert( value != NULL );
	assert( value->raw != NULL );
	assert( value->bytes > 0 );
	if( value->kind == TOKEN_NUMBER ){
/* bounds check */
		value++;
	} else if( value->kind == TOKEN_REGISTER ){
		validate_not_a_type( procedure );
		validate_is_a_register( procedure, value );
		value++;
	} else if( value->kind == TOKEN_ADD ){
		value = validate_value( procedure, value + 1 );
		value = validate_value( procedure, value );
	} else {
		compiler_error( *value, "Invalide value" );
	}
	return value;
}

void validate_procedure( syntax_token* procedure ){
	assert( procedure != NULL );
	assert( procedure->raw != NULL );
	assert( procedure->bytes > 0 );
	assert( procedure->sibling > 0 );
	assert( procedure->kind == TOKEN_PROCEDURE );
	validate_exclusive_global( procedure );
	validate_not_a_type( procedure );
	syntax_token* token = &procedure[ 1 ];
	assert( token->kind == TOKEN_TYPE );
	validate_is_a_type( token );
	token++;
	assert( token->kind == TOKEN_ROUTINE );
	validate_not_a_type( token );
	validate_exclusive_routine( procedure, token );
	token++;
	while( token->kind == TOKEN_ARGUMENT ){
		validate_not_a_type( token );
		validate_exclusive_register( procedure, token );
		token++;
		assert( token->kind == TOKEN_TYPE );
		validate_is_a_type( token );
		token++;
	}
	assert( token->kind == TOKEN_RETURN || token->kind == TOKEN_REGISTER );
	while( token < &procedure[ procedure->sibling ]){
		if( token->kind == TOKEN_REGISTER ){
			validate_not_a_type( token );
			validate_exclusive_register( procedure, token );
			token++;
			validate_is_a_type( token );
			token++;
			token = validate_value( procedure, token );
		} else if( token->kind == TOKEN_RETURN ){
			token++;
			token = validate_value( procedure, token );
			assert( token->kind == TOKEN_ERROR );
			break;
		} else {
			assert( 0 );
		}
	}
	procedures_finished_validating++;
}

void push_output(){
/* critical section */
	i32 error = pthread_mutex_lock( &output_mutex );
	assert( !error );
	memcpy( &output_buffer[ output_buffer_count ], scratch, scratch_count );
	output_buffer_count += scratch_count;
	error = pthread_mutex_unlock( &output_mutex );
/* end critical section */
	assert( !error );
	scratch_count = 0;
}

void output_llvmir_procedure( syntax_token* procedure ){
	assert( procedure != NULL );
	assert( procedure->raw != NULL );
	assert( procedure->bytes > 0 );
	assert( procedure->sibling > 0 );
	assert( procedure->kind == TOKEN_PROCEDURE );
	while( 1 ){
		assert( procedure->bytes == 5 && strncmp( procedure->raw, "start", 5 ) == 0 );
		i32 return_type_index = scratch_count + 7;
		buffer_append( (char*) scratch, &scratch_count, "define  @start( ", 16 );
		syntax_token* return_type_token = procedure + 1;
		assert( return_type_token->kind == TOKEN_TYPE );
		buffer_insert( (char*) scratch, &scratch_count, return_type_index, return_type_token->raw, return_type_token->bytes );
		syntax_token* token = return_type_token + 1;
		assert( token->kind == TOKEN_ROUTINE );
		token++;
		if( token->kind == TOKEN_ARGUMENT ){
			token++;
			assert( token->kind == TOKEN_TYPE );
			buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
			buffer_append( (char*) scratch, &scratch_count, " %", 2 );
			buffer_append( (char*) scratch, &scratch_count, token[ -1 ].raw, token[ -1 ].bytes );
			token++;
		}
		buffer_append( (char*) scratch, &scratch_count, " ){\n", 4 );
		while( 1 ){
			if( token->kind == TOKEN_REGISTER ){
				buffer_append( (char*) scratch, &scratch_count, "\t%", 2 );
				buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				token = token + 1;
				assert( token->kind == TOKEN_TYPE );
				token = token + 1;
				if( token->kind == TOKEN_NUMBER ){
					buffer_append( (char*) scratch, &scratch_count, " = add ", 7 );
					buffer_append( (char*) scratch, &scratch_count, token[ -1 ].raw, token[ -1 ].bytes );
					buffer_append( (char*) scratch, &scratch_count, " 0, ", 4 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else if( token->kind == TOKEN_REGISTER ){
					buffer_append( (char*) scratch, &scratch_count, " = add ", 7 );
					buffer_append( (char*) scratch, &scratch_count, token[ -1 ].raw, token[ -1 ].bytes );
					buffer_append( (char*) scratch, &scratch_count, " 0, %", 5 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else if( token->kind == TOKEN_ADD ){
					buffer_append( (char*) scratch, &scratch_count, " = add ", 7 );
					buffer_append( (char*) scratch, &scratch_count, token[ -1 ].raw, token[ -1 ].bytes );
					token = token + 1;
					if( token->kind == TOKEN_NUMBER ){
						buffer_append( (char*) scratch, &scratch_count, " ", 1 );
						buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
					} else if( token->kind == TOKEN_REGISTER ){
						buffer_append( (char*) scratch, &scratch_count, " %", 2 );
						buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
					} else {
						assert( 0 );
					}
					token = token + 1;
					if( token->kind == TOKEN_NUMBER ){
						buffer_append( (char*) scratch, &scratch_count, ", ", 2 );
						buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
					} else if( token->kind == TOKEN_REGISTER ){
						buffer_append( (char*) scratch, &scratch_count, ", %", 3 );
						buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
					} else {
						assert( 0 );
					}
				} else {
					assert( 0 );
				}
				buffer_append( (char*) scratch, &scratch_count, "\n", 1 );
			} else if( token->kind == TOKEN_RETURN ){
				buffer_append( (char*) scratch, &scratch_count, "\tret ", 5 );
				buffer_append( (char*) scratch, &scratch_count, return_type_token->raw, return_type_token->bytes );
				token = token + 1;
				if( token->kind == TOKEN_NUMBER ){
					buffer_append( (char*) scratch, &scratch_count, " ", 1 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else if( token->kind == TOKEN_REGISTER ){
					buffer_append( (char*) scratch, &scratch_count, " %", 2 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else {
					assert( 0 );
				}
				buffer_append( (char*) scratch, &scratch_count, "\n}\n", 3 );
				break;
			} else {
				assert( 0 );
			}
			token++;
		}
		push_output();
		break;
	}
	procedures_finished_outputing++;
}

void output_procedure( syntax_token* procedure ){
	output_llvmir_procedure( procedure );
}

void* thread_main( void* argt ){
	i64 thread_index = (i64) argt;
	while( 1 ){
/*
  If a file is ready to be parsed, go parse it.
  If all files have been parsed, break out of the loop.
  Else unlock and try again.
*/
/* critical section */
		i32 error = pthread_mutex_lock( &file_mutex );
		assert( !error );
		assert( file_buffer_count > 0 );
		assert( files_started_parsing >= 0 );
		assert( files_finished_parsing >= 0 );
		assert( file_buffer_count >= files_started_parsing );
		assert( file_buffer_count >= files_finished_parsing );
		assert( files_started_parsing >= files_finished_parsing );
		if( file_buffer_count == files_finished_parsing ){
			error = pthread_mutex_unlock( &file_mutex );
			assert( !error );
			break;
		}
		if( files_started_parsing < file_buffer_count ){
			i32 index = files_started_parsing;
			files_started_parsing++;
			error = pthread_mutex_unlock( &file_mutex );
			assert( !error );
			parse_file( &file_buffer[ index ]);
		} else {
			error = pthread_mutex_unlock( &file_mutex );
			assert( !error );
		}
/* end critical section */
	}
	while( 1 ){
/* critical section */
		i32 error = pthread_mutex_lock( &procedure_mutex );
		assert( !error );
		assert( procedure_count >= procedures_started_validating );
		assert( procedure_count >= procedures_finished_validating );
		assert( procedures_started_validating >= procedures_finished_validating );
		if( procedure_count == procedures_finished_validating ){
			error = pthread_mutex_unlock( &procedure_mutex );
			assert( !error );
			break;
		}
		if( procedures_started_validating < procedure_count ){
			i32 index = procedure_buffer_index_validating;
			assert( procedure_buffer[ index ].kind == TOKEN_PROCEDURE );
			procedure_buffer_index_validating += procedure_buffer[ index ].sibling;
			procedures_started_validating++;
			error = pthread_mutex_unlock( &procedure_mutex );
			assert( !error );
			validate_procedure( &procedure_buffer[ index ]);
		} else {
			error = pthread_mutex_unlock( &procedure_mutex );
			assert( !error );
		}
/* end critical section */
	}
	while( 1 ){
/* critical section */
		i32 error = pthread_mutex_lock( &output_mutex );
		assert( !error );
		assert( procedure_count >= procedures_started_outputing );
		assert( procedure_count >= procedures_finished_outputing );
		assert( procedures_started_outputing >= procedures_finished_outputing );
		if( procedure_count == procedures_finished_outputing ){
			error = pthread_mutex_unlock( &output_mutex );
			assert( !error );
			break;
		}
		if( procedures_started_outputing < procedure_count ){
			i32 index = procedure_buffer_index_outputed;
			assert( procedure_buffer[ index ].kind == TOKEN_PROCEDURE );
			procedure_buffer_index_outputed += procedure_buffer[ index ].sibling;
			procedures_started_outputing++;
			error = pthread_mutex_unlock( &output_mutex );
			assert( !error );
			output_procedure( &procedure_buffer[ index ]);
		} else {
			error = pthread_mutex_unlock( &output_mutex );
			assert( !error );
		}
/* end critical section */
	}
	if( thread_index == 0 ){
		char* target = "target triple = \"x86_64-unknown-linux-gnu\"\n\n";
		buffer_insert( output_buffer, &output_buffer_count, 0, target, strlen( target ));
		char* output_name = __builtin_alloca( file_buffer[ 0 ].name_bytes + 4 );
		memcpy( output_name, file_buffer[ 0 ].name, file_buffer[ 0 ].name_bytes );
		memcpy( &output_name[ file_buffer[ 0 ].name_bytes ], ".ll\0", 4 );
		FILE* fp = fopen( output_name, "w" );
		i32 write_bytes = fwrite( output_buffer, 1, output_buffer_count, fp );
		assert( write_bytes == output_buffer_count );
		fclose( fp );
	}
	return NULL;
}

i32 main( i32 argc, char* argv[] ){
	if( argc != 2 ){
		error( "Usage: ssal_llvmir <filename>" );
	}
	pthread_mutex_init( &file_mutex, NULL );
	i32 name_bytes = strlen( argv[ 1 ]);
	file_add( argv[ 1 ], name_bytes );
#ifdef SINGLE_THREAD
	thread_main( (void*) ( 0 ));
#else /* default: use multiple threads */
	i64 cpu_count = sysconf( _SC_NPROCESSORS_ONLN );
	assert( cpu_count > 0 );
	pthread_t thread_array[ cpu_count - 1 ];
	i64 i = 0;
	while( i < cpu_count - 1 ){
		i32 error = pthread_create( &thread_array[ i ], NULL, thread_main, (void*) i );
		assert( !error );
		i += 1;
	}
	thread_main( (void*) ( cpu_count - 1 ));
	i = 0;
	while( i < cpu_count - 1 ){
		pthread_join( thread_array[ i ], NULL );
		i += 1;
	}
#endif /* threading */
	print_tokens();
	return 0;
}
